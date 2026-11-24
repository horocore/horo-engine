#include "PackageCliHost.h"

#include "Horo/Application/ProjectAssetRegistrySource.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "PackageBehaviorReferences.h"
#include "PackageProjectSession.h"
#include "ProjectProtocolIdentity.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <map>
#include <thread>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Reads the parser's typed path instead of reparsing command-line text. */
        Result<std::filesystem::path> PathOption(const Cli::CliCommandRequest &request, const char *name) {
            const auto option = std::ranges::find(request.options, name, &Cli::CliParsedOption::name);
            if (option == request.options.end() || option->values.size() != 1)
                return Result<std::filesystem::path>::Failure(MakeError(Cli::CliErrors::ParseFailed));
            const auto *path = std::get_if<Cli::CliPathValue>(&option->values.front().value);
            if (path == nullptr)
                return Result<std::filesystem::path>::Failure(MakeError(Cli::CliErrors::ParseFailed));
            auto result = std::filesystem::path{std::u8string{path->value.begin(), path->value.end()}};
            if (!result.is_absolute() || result != result.lexically_normal())
                return Result<std::filesystem::path>::Failure(MakeError(Cli::CliErrors::ParseFailed));
            return Result<std::filesystem::path>::Success(std::move(result));
        }

        /** @brief Identifies recoverable preparation without swallowing original policy or filesystem diagnostics. */
        bool Preparing(const Error &error) {
            return error.domain.Value() == "packages.host" &&
                   (error.code.Value() == "packages.host.preparing" || error.code.Value() == "packages.host.busy" ||
                    error.code.Value() == "packages.host.retirement_busy");
        }

        constexpr std::size_t UpdatePreviewPageSize = 32;
        enum class PackageOperation {
            Forward,
            Rollback,
            Removal
        };

        struct CandidateDocuments final {
            std::string request;
            std::string lock;
            std::optional<Packages::HoroPackageId> removalPackage;
        };

        struct InspectedPackageUpdate final {
            PackageProjectPolicy policy;
            PreparedPackageProject base;
            PackageUpdateApproval approval;
            std::string request;
            std::string lock;
            Sha256Digest requestHash;
            Packages::PackageUpdatePreview preview;
            PackageOperation operation;
            std::optional<Packages::HoroPackageId> removalPackage;

            /** @brief Identifies the reviewed project and exact old and proposed documents. */
            std::string ApprovalFingerprint() const {
                constexpr std::array commands{"update", "rollback", "remove"};
                const auto command = commands[static_cast<std::size_t>(operation)];
                const auto bytes = std::format("horo.package.{}.v1 {} {} {} {} {}", command, ProjectProtocolIdentity(policy.projectRoot),
                                               FormatSha256(approval.baseRequest), FormatSha256(approval.baseLock),
                                               FormatSha256(approval.candidateRequest), FormatSha256(approval.candidateLock));
                const auto identity = removalPackage ? std::format("{} {}", bytes, removalPackage->Value()) : bytes;
                return FormatSha256(ComputeSha256(std::as_bytes(std::span{identity})));
            }
        };

        /** @brief Reads an optional, bounded row offset from the dispatcher. */
        Result<std::size_t> PreviewOffset(const Cli::CliCommandRequest &request) {
            const auto option = std::ranges::find(request.options, "offset", &Cli::CliParsedOption::name);
            if (option == request.options.end())
                return Result<std::size_t>::Success(0);
            if (option->values.size() == 1) {
                if (const auto *value = std::get_if<std::int64_t>(&option->values.front().value); value && *value >= 0 && *value <= 1024)
                    return Result<std::size_t>::Success(static_cast<std::size_t>(*value));
            }
            return Result<std::size_t>::Failure(MakeError(Cli::CliErrors::ParseFailed));
        }

        /** @brief Projects identity and provenance without including bytes or source locators. */
        std::string PreviewEntry(const std::optional<Packages::LockedPackage> &entry) {
            if (!entry)
                return "absent";
            return std::format("{} {} {}", entry->version.ToString(), entry->source.Value(), FormatSha256(entry->artifactDigest));
        }

        /** @brief Presents one page of ordered changes within the CLI's bounded scalar output contract. */
        Cli::CliCommandResult EncodePreview(const InspectedPackageUpdate &update, const std::size_t offset) {
            const auto &preview = update.preview;
            const auto changes = std::span{preview.changes};
            const auto start = std::min(offset, changes.size());
            const auto page = changes.subspan(start, std::min(UpdatePreviewPageSize, changes.size() - start));
            std::string rows;
            for (const auto &change : page) {
                const auto &package = change.current ? change.current->package : change.candidate->package;
                if (!rows.empty())
                    rows += '\n';
                rows += std::format("{} [{}] -> [{}]", package.Value(), PreviewEntry(change.current), PreviewEntry(change.candidate));
            }
            const auto end = start + page.size();
            return {.fields = {{"requestChanged", preview.requestChanged},
                               {"totalChanges", static_cast<std::int64_t>(changes.size())},
                               {"nextOffset", end < changes.size() ? static_cast<std::int64_t>(end) : -1},
                               {"changes", std::move(rows)},
                               {"approvalFingerprint", update.ApprovalFingerprint()}}};
        }

        /** @brief Reads a complete explicitly selected candidate without mutating the project. */
        Result<CandidateDocuments> ReadCandidate(const Cli::CliCommandRequest &request, NativeDurableFileSystem &files,
                                                 const std::size_t lockLimit, const CancellationToken &cancellation) {
            using Output = Result<CandidateDocuments>;
            const auto requestPath = PathOption(request, "candidate-request");
            if (requestPath.HasError())
                return Output::Failure(requestPath.ErrorValue());
            const auto lockPath = PathOption(request, "candidate-lock");
            if (lockPath.HasError())
                return Output::Failure(lockPath.ErrorValue());
            const auto intent = files.ReadFileBounded(requestPath.Value(), 1024 * 1024, cancellation);
            if (intent.HasError())
                return Output::Failure(intent.ErrorValue());
            const auto lock = files.ReadFileBounded(lockPath.Value(), lockLimit, cancellation);
            if (lock.HasError())
                return Output::Failure(lock.ErrorValue());
            return Output::Success(
                {std::string{intent.Value().begin(), intent.Value().end()}, std::string{lock.Value().begin(), lock.Value().end()}});
        }

        /** @brief Reads a retained point that still belongs to the same inspected current pair. */
        Result<CandidateDocuments> ReadRollback(const PackageProjectPolicy &policy, const PreparedPackageProject &base,
                                                NativeDurableFileSystem &files, const CancellationToken &cancellation) {
            using Output = Result<CandidateDocuments>;
            auto point = ReadPackageProjectRollback(policy, files, cancellation);
            if (point.HasError())
                return Output::Failure(point.ErrorValue());
            if (point.Value().publishedRequest != base.requestDocumentDigest || point.Value().publishedLock != base.lockDigest)
                return Output::Failure(Error{ErrorCode{"packages.host.metadata_conflict"}, ErrorDomainId{"packages.host"}});
            auto documents = std::move(point).Value();
            return Output::Success({std::move(documents.request), std::move(documents.lock)});
        }

        /** @brief Reads a single explicitly selected portable package ID. */
        Result<Packages::HoroPackageId> RemovalPackage(const Cli::CliCommandRequest &request) {
            const auto option = std::ranges::find(request.options, "package-id", &Cli::CliParsedOption::name);
            if (option != request.options.end() && option->values.size() == 1) {
                if (const auto *id = std::get_if<std::string>(&option->values.front().value))
                    return Packages::HoroPackageId::Parse(*id);
            }
            return Result<Packages::HoroPackageId>::Failure(MakeError(Cli::CliErrors::ParseFailed));
        }

        /** @brief Derives the remaining graph without resolving new versions or touching saved project files. */
        Result<CandidateDocuments> PlanRemoval(const PreparedPackageProject &base, const Packages::HoroPackageId &package) {
            const auto &application = base.application;
            const auto plan = Packages::PackageRemovalPlan::Prepare(*application.portableRequest, *application.lockedPlan, package);
            if (plan.HasError())
                return Result<CandidateDocuments>::Failure(plan.ErrorValue());
            return Result<CandidateDocuments>::Success(
                {plan.Value().request.SerializeCanonical(), plan.Value().lock.SerializeCanonical(), package});
        }

        /** @brief Selects the exact candidate for the requested package operation. */
        Result<CandidateDocuments> InspectCandidate(const Cli::CliCommandRequest &request, const PackageProjectPolicy &policy,
                                                    const PreparedPackageProject &base, const PackageOperation operation,
                                                    const CancellationToken &cancellation, NativeDurableFileSystem &files) {
            if (operation == PackageOperation::Removal) {
                auto selected = RemovalPackage(request);
                if (selected.HasError())
                    return Result<CandidateDocuments>::Failure(selected.ErrorValue());
                return PlanRemoval(base, selected.Value());
            }
            if (operation == PackageOperation::Rollback)
                return ReadRollback(policy, base, files, cancellation);
            return ReadCandidate(request, files, policy.limits.lockfile.documentBytes, cancellation);
        }

        /** @brief Compares bounded candidate documents to the live, read-only project metadata snapshot. */
        Result<InspectedPackageUpdate> InspectUpdate(const Cli::CliCommandRequest &request, const PackageOperation operation,
                                                     const CancellationToken &cancellation) {
            using Output = Result<InspectedPackageUpdate>;
            const auto root = PathOption(request, "project");
            if (root.HasError())
                return Output::Failure(root.ErrorValue());
            auto platform = NativePackagePlatform();
            if (platform.HasError())
                return Output::Failure(platform.ErrorValue());
            PackageProjectPolicy policy{.projectRoot = root.Value(), .platform = std::move(platform).Value()};
            NativeDurableFileSystem files;
            auto prepared = PreparePackageProject(policy, files, cancellation, PackageProjectReadMode::ReadOnly);
            if (prepared.HasError())
                return Output::Failure(prepared.ErrorValue());
            auto documents = InspectCandidate(request, policy, prepared.Value(), operation, cancellation, files);
            if (documents.HasError())
                return Output::Failure(documents.ErrorValue());
            auto [candidateRequest, candidateLock, removalPackage] = std::move(documents).Value();
            const auto candidate = Packages::ValidatedPackageRequest::Parse(candidateRequest);
            if (candidate.HasError())
                return Output::Failure(candidate.ErrorValue());
            const auto lock = Packages::ValidatedPackageLockfileV1::Parse(candidateLock, policy.limits.lockfile);
            if (lock.HasError())
                return Output::Failure(lock.ErrorValue());
            if (const auto supported = lock.Value().ValidateForRestore(candidate.Value().Digest(), policy.platform,
                                                                       policy.limits.supportedPackageFormatVersion);
                supported.HasError())
                return Output::Failure(supported.ErrorValue());
            const auto &base = prepared.Value().application;
            auto preview =
                Packages::PackageUpdatePreview::Prepare(*base.portableRequest, *base.lockedPlan, candidate.Value(), lock.Value());
            if (preview.HasError())
                return Output::Failure(preview.ErrorValue());
            const PackageUpdateApproval approval{prepared.Value().requestDocumentDigest, prepared.Value().lockDigest,
                                                 ComputeSha256(std::as_bytes(std::span{candidateRequest})),
                                                 ComputeSha256(std::as_bytes(std::span{candidateLock}))};
            return Output::Success({std::move(policy), std::move(prepared).Value(), approval, std::move(candidateRequest),
                                    std::move(candidateLock), candidate.Value().Digest(), std::move(preview).Value(), operation,
                                    std::move(removalPackage)});
        }

        class PackageUpdatePreviewAdapter final : public Cli::ICliCommandAdapter {
        public:
            PackageUpdatePreviewAdapter(Cli::CliCommandDescriptor descriptor, const PackageOperation operation)
                : descriptor_(std::move(descriptor)), operation_(operation) {}

            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<Cli::CliCommandResult> Execute(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &context) override {
                using Output = Result<Cli::CliCommandResult>;
                const auto offset = PreviewOffset(request);
                if (offset.HasError())
                    return Output::Failure(offset.ErrorValue());
                const auto preview = InspectUpdate(request, operation_, context.Cancellation());
                if (preview.HasError())
                    return Output::Failure(preview.ErrorValue());
                return Output::Success(EncodePreview(preview.Value(), offset.Value()));
            }

        private:
            Cli::CliCommandDescriptor descriptor_;
            PackageOperation operation_;
        };

        /** @brief Reads the explicitly supplied preview fingerprint. */
        std::string_view UpdateApproval(const Cli::CliCommandRequest &request) {
            const auto option = std::ranges::find(request.options, "approve", &Cli::CliParsedOption::name);
            if (option == request.options.end() || option->values.size() != 1)
                return {};
            const auto *value = std::get_if<std::string>(&option->values.front().value);
            return value ? std::string_view{*value} : std::string_view{};
        }

        /** @brief Reports the stop authority that terminated a package command. */
        Error UpdateStopped(const Cli::CliExecutionContext &context) {
            const auto remaining = context.RemainingTimeout();
            return MakeError(remaining && remaining->count() == 0 ? Cli::CliErrors::ExecutionTimedOut : Cli::CliErrors::ExecutionCancelled);
        }

        /** @brief Waits on the CLI lane for the exact offline verification operation. */
        Result<Packages::StagedPackageRestoreRecord> StageWithCache(const Packages::PackageRestoreRequest &request,
                                                                    NativeDurableFileSystem &files, Packages::PackageCacheStore &cache,
                                                                    const Cli::CliExecutionContext &context) {
            using Output = Result<Packages::StagedPackageRestoreRecord>;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto service = Packages::PackageRestoreService::Create(jobs, files, cache);
            if (service.HasError())
                return Output::Failure(service.ErrorValue());
            auto restore = std::move(service).Value();
            const auto started = restore.StartStaged(request);
            if (started.HasError())
                return Output::Failure(started.ErrorValue());
            const auto operation = started.Value().Id();
            while (!context.IsStopRequested()) {
                const auto progress = restore.Query(operation);
                if (progress && progress->outcome == Packages::PackageRestoreOutcome::Running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    continue;
                }
                if (auto staged = restore.Staged(operation))
                    return Output::Success(std::move(*staged));
                if (progress && progress->diagnostic)
                    return Output::Failure(*progress->diagnostic);
                return Output::Failure(Error{ErrorCode{"packages.host.staging_unavailable"}, ErrorDomainId{"packages.host"}});
            }
            return Output::Failure(UpdateStopped(context));
        }

        /** @brief Rejects changes to selected versions, source, policy, or exported contribution classes. */
        bool CanPreserveAssetReferences(const Packages::PackageUpdateChange &change) {
            if (!change.current)
                return true;
            if (!change.candidate)
                return false;
            const auto &next = *change.candidate;
            auto compatible = *change.current;
            compatible.artifactDigest = next.artifactDigest;
            compatible.fileManifestDigest = next.fileManifestDigest;
            return compatible == next && std::ranges::equal(next.contributions, std::array<std::string_view, 1>{"assets"});
        }

        /** @brief Verifies that every old content path retains its bytes and contribution owner. */
        bool PreservesAssetFiles(const Packages::ValidatedPackageArchive &previous, const Packages::ValidatedPackageArchive &candidate) {
            const auto contributionPath = [](const Packages::PackageFileEntry &file) -> std::string_view {
                return file.contributionRoot ? std::string_view{file.contributionRoot->Value()} : std::string_view{};
            };
            std::map<std::string_view, const Packages::PackageFileEntry *, std::less<>> byPath;
            for (const auto &file : candidate.Manifest().Entries()) {
                const std::string_view path = file.id.path.Value();
                if (file.executable || (path != "horo-package.toml" && !path.starts_with("assets/")))
                    return false;
                byPath.emplace(path, &file);
            }
            return std::ranges::all_of(previous.Manifest().Entries(), [&byPath, &contributionPath](const auto &oldFile) {
                const auto next = byPath.find(oldFile.id.path.Value());
                return next != byPath.end() && !oldFile.executable && oldFile.digest == next->second->digest &&
                       oldFile.size == next->second->size && contributionPath(oldFile) == contributionPath(*next->second);
            });
        }

        Error UpdateNeedsMigration() {
            return Error{ErrorCode{"packages.host.update_requires_migration_review"}, ErrorDomainId{"packages.host"}};
        }

        /** @brief Carries the admitted command deadline into project reads and publication. */
        Platform::ProjectReadContext ReadAuthority(const PackageProjectPolicy &policy, const Cli::CliExecutionContext &context) {
            const auto remaining = context.RemainingTimeout();
            return {.projectIdentity = ProjectProtocolIdentity(policy.projectRoot),
                    .projectGeneration = 1,
                    .cancellation = context.Cancellation(),
                    .deadline = remaining ? std::chrono::steady_clock::now() + *remaining : std::chrono::steady_clock::time_point::max(),
                    .authorityStopped = [&context] { return context.IsStopRequested(); }};
        }

        /** @brief Publishes an approved offline withdrawal after inspecting the real root-bound project inventory. */
        Result<Cli::CliCommandResult> ApplyRemoval(const InspectedPackageUpdate &update, NativeDurableFileSystem &files,
                                                   Packages::PackageCacheStore &cache, const Cli::CliExecutionContext &context) {
            using Output = Result<Cli::CliCommandResult>;
            auto restore = update.base.application.restore;
            restore.cancellation = context.Cancellation();
            const auto verified = StageWithCache(restore, files, cache, context);
            if (verified.HasError())
                return Output::Failure(verified.ErrorValue());
            const auto admission = ReadAuthority(update.policy, context);
            const auto read =
                Platform::CreateNativeProjectReadFiles(update.policy.projectRoot, admission.projectIdentity, admission.projectGeneration);
            if (read.HasError())
                return Output::Failure(read.ErrorValue());
            const auto assets = CreateProjectAssetRegistrySource(read.Value(), admission);
            if (assets.HasError())
                return Output::Failure(assets.ErrorValue());
            const PackageRemovalSources sources{.assets = *assets.Value(),
                                                .files = *read.Value(),
                                                .context = admission,
                                                .offline = &verified.Value()};
            const auto published = PublishPackageProjectRemoval(update.policy, update.base, *update.removalPackage, files, sources);
            if (published.HasError())
                return Output::Failure(published.ErrorValue());
            return Output::Success({.fields = {{"recoveryRequired", published.Value().recoveryDiagnostic.has_value()}}});
        }

        /** @brief Checks the exact old and staged archives for each replaced asset package before publication. */
        Result<void> CheckAssetReplacements(const InspectedPackageUpdate &update, const Packages::PackageRestoreGraph &staged,
                                            Packages::PackageCacheStore &cache) {
            for (const auto &change : update.preview.changes) {
                if (!change.current)
                    continue;
                const auto &old = *change.current;
                const auto next = std::ranges::find_if(staged.packages, [&old](const auto &item) {
                    return item.lock.package == old.package;
                });
                if (next == staged.packages.end() || !next->archive)
                    return Result<void>::Failure(UpdateNeedsMigration());
                const auto previous = cache.Load(old.artifactDigest);
                if (previous.HasError())
                    return Result<void>::Failure(previous.ErrorValue());
                if (!previous.Value() || previous.Value()->PackageManifestDigest() != old.manifestDigest ||
                    previous.Value()->Manifest().Digest() != old.fileManifestDigest ||
                    !PreservesAssetFiles(*previous.Value(), *next->archive))
                    return Result<void>::Failure(UpdateNeedsMigration());
            }
            return Result<void>::Success();
        }

        /** @brief Verifies and publishes a candidate whose existing package references remain valid. */
        Result<Cli::CliCommandResult> PublishStagedUpdate(const InspectedPackageUpdate &update, NativeDurableFileSystem &files,
                                                          Packages::PackageCacheStore &cache, const Cli::CliExecutionContext &context) {
            using Output = Result<Cli::CliCommandResult>;
            const auto staged = StageWithCache({.projectRoot = update.policy.projectRoot,
                                                .lockfileJson = update.lock,
                                                .expectedRequestHash = update.requestHash,
                                                .platform = update.policy.platform,
                                                .mode = update.policy.mode,
                                                .cancellation = context.Cancellation()},
                                               files, cache, context);
            if (staged.HasError())
                return Output::Failure(staged.ErrorValue());
            if (const auto content = CheckAssetReplacements(update, *staged.Value().Graph(), cache); content.HasError())
                return Output::Failure(content.ErrorValue());

            const auto admission = ReadAuthority(update.policy, context);
            const ApprovedPackageProjectUpdate approved{update.base, staged.Value(), update.approval, update.request, update.lock};
            const auto published = PublishPackageProjectUpdate(update.policy, approved, files, admission);
            if (published.HasError())
                return Output::Failure(published.ErrorValue());
            return Output::Success({.fields = {{"recoveryRequired", published.Value().recoveryDiagnostic.has_value()}}});
        }

        /** @brief Applies an approved update after proving its package references remain usable. */
        Result<Cli::CliCommandResult> ApplyUpdate(const Cli::CliCommandRequest &request, const InspectedPackageUpdate &update,
                                                  const Cli::CliExecutionContext &context) {
            using Output = Result<Cli::CliCommandResult>;
            if (UpdateApproval(request) != update.ApprovalFingerprint())
                return Output::Failure(Error{ErrorCode{"packages.host.update_approval_mismatch"}, ErrorDomainId{"packages.host"}});
            if (!update.removalPackage && !std::ranges::all_of(update.preview.changes, CanPreserveAssetReferences))
                return Output::Failure(UpdateNeedsMigration());
            const auto cachePath = PathOption(request, "package-cache-root");
            if (cachePath.HasError())
                return Output::Failure(cachePath.ErrorValue());
            if (context.IsStopRequested())
                return Output::Failure(UpdateStopped(context));

            NativeDurableFileSystem files;
            auto cache = Packages::PackageCacheStore::Create(files, cachePath.Value());
            if (cache.HasError())
                return Output::Failure(cache.ErrorValue());
            auto verifiedCache = std::move(cache).Value();
            if (update.removalPackage)
                return ApplyRemoval(update, files, verifiedCache, context);
            return PublishStagedUpdate(update, files, verifiedCache, context);
        }

        class PackageApplyUpdateAdapter final : public Cli::ICliCommandAdapter {
        public:
            PackageApplyUpdateAdapter(Cli::CliCommandDescriptor descriptor, const PackageOperation operation)
                : descriptor_(std::move(descriptor)), operation_(operation) {}

            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<Cli::CliCommandResult> Execute(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &context) override {
                const auto update = InspectUpdate(request, operation_, context.Cancellation());
                if (update.HasError())
                    return Result<Cli::CliCommandResult>::Failure(update.ErrorValue());
                return ApplyUpdate(request, update.Value(), context);
            }

        private:
            Cli::CliCommandDescriptor descriptor_;
            PackageOperation operation_;
        };
    }  // namespace

    /** @copydoc DescribePackageUpdatePreviewCommand */
    Cli::CliCommandDescriptor DescribePackageUpdatePreviewCommand() {
        using namespace Cli;
        return {.path = {{"package", "preview-update"}},
                .summary = "Compare a project's current packages with an explicitly resolved candidate.",
                .options = {{.name = "candidate-lock",
                             .summary = "Absolute path to the resolved candidate lock.",
                             .valueKind = CliOptionValueKind::Path,
                             .required = true},
                            {.name = "candidate-request",
                             .summary = "Absolute path to the portable candidate request.",
                             .valueKind = CliOptionValueKind::Path,
                             .required = true},
                            {.name = "offset",
                             .summary = "Zero-based offset into the ordered changes.",
                             .valueKind = CliOptionValueKind::SignedInteger,
                             .numericRange = {.minimumInteger = 0, .maximumInteger = 1024}}},
                .requiredCapabilities = {{"horo.package.manage"}},
                .output = {.id = "horo.package.preview-update",
                           .version = 1,
                           .formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines},
                .interactive = CliInteractivePolicy::Forbidden,
                .hosts = CliHostAvailability::HoroEngine,
                .contractVersion = {1, 0, 0},
                .sideEffects = CliSideEffectPolicy::ReadsState,
                .cancellation = CliCancellationPolicy::Cooperative,
                .timeout = {.maximumMilliseconds = 86'400'000},
                .ownerId = "horo.package.cli"};
    }

    /** @copydoc MakePackageUpdatePreviewCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePackageUpdatePreviewCommandAdapter() {
        return {.adapter = std::make_unique<PackageUpdatePreviewAdapter>(DescribePackageUpdatePreviewCommand(), PackageOperation::Forward),
                .capabilities = {{"horo.package.manage"}}};
    }

    /** @copydoc DescribePackageApplyUpdateCommand */
    Cli::CliCommandDescriptor DescribePackageApplyUpdateCommand() {
        using namespace Cli;
        auto descriptor = DescribePackageUpdatePreviewCommand();
        descriptor.path = {{"package", "apply-update"}};
        descriptor.summary = "Apply an approved offline update that preserves existing package references.";
        descriptor.options.pop_back();
        descriptor.options.insert(descriptor.options.begin(), {.name = "approve",
                                                               .summary = "Exact fingerprint from the update preview.",
                                                               .valueKind = CliOptionValueKind::String,
                                                               .required = true});
        descriptor.options.push_back({.name = "package-cache-root",
                                      .summary = "Absolute root of the verified package cache.",
                                      .valueKind = CliOptionValueKind::Path,
                                      .required = true});
        descriptor.output.id = "horo.package.apply-update";
        descriptor.sideEffects = CliSideEffectPolicy::WritesFiles;
        return descriptor;
    }

    /** @copydoc MakePackageApplyUpdateCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePackageApplyUpdateCommandAdapter() {
        return {.adapter = std::make_unique<PackageApplyUpdateAdapter>(DescribePackageApplyUpdateCommand(), PackageOperation::Forward),
                .capabilities = {{"horo.package.manage"}}};
    }

    /** @copydoc DescribePackageRollbackPreviewCommand */
    Cli::CliCommandDescriptor DescribePackageRollbackPreviewCommand() {
        auto descriptor = DescribePackageUpdatePreviewCommand();
        descriptor.path = {{"package", "preview-rollback"}};
        descriptor.summary = "Review the previous package metadata retained by the latest update.";
        std::erase_if(descriptor.options, [](const auto &option) {
            return option.name.starts_with("candidate-");
        });
        descriptor.output.id = "horo.package.preview-rollback";
        return descriptor;
    }

    /** @copydoc DescribePackageRollbackUpdateCommand */
    Cli::CliCommandDescriptor DescribePackageRollbackUpdateCommand() {
        auto descriptor = DescribePackageApplyUpdateCommand();
        descriptor.path = {{"package", "rollback-update"}};
        descriptor.summary = "Restore approved prior package metadata when existing references are preserved.";
        std::erase_if(descriptor.options, [](const auto &option) {
            return option.name.starts_with("candidate-");
        });
        descriptor.output.id = "horo.package.rollback-update";
        return descriptor;
    }

    /** @copydoc MakePackageRollbackPreviewCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePackageRollbackPreviewCommandAdapter() {
        return {.adapter =
                    std::make_unique<PackageUpdatePreviewAdapter>(DescribePackageRollbackPreviewCommand(), PackageOperation::Rollback),
                .capabilities = {{"horo.package.manage"}}};
    }

    /** @copydoc MakePackageRollbackUpdateCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePackageRollbackUpdateCommandAdapter() {
        return {.adapter = std::make_unique<PackageApplyUpdateAdapter>(DescribePackageRollbackUpdateCommand(), PackageOperation::Rollback),
                .capabilities = {{"horo.package.manage"}}};
    }

    /** @copydoc DescribePackageRemovalPreviewCommand */
    Cli::CliCommandDescriptor DescribePackageRemovalPreviewCommand() {
        auto descriptor = DescribePackageRollbackPreviewCommand();
        descriptor.path = {{"package", "preview-remove"}};
        descriptor.summary = "Review the exact package withdrawal and its approval fingerprint.";
        descriptor.options.insert(descriptor.options.begin(), {.name = "package-id",
                                                               .summary = "Portable ID of a directly requested package.",
                                                               .valueKind = Cli::CliOptionValueKind::String,
                                                               .required = true});
        descriptor.output.id = "horo.package.preview-remove";
        return descriptor;
    }

    /** @copydoc DescribePackageApplyRemovalCommand */
    Cli::CliCommandDescriptor DescribePackageApplyRemovalCommand() {
        auto descriptor = DescribePackageRollbackUpdateCommand();
        descriptor.path = {{"package", "remove"}};
        descriptor.summary = "Withdraw an approved asset package from a local project.";
        descriptor.options.push_back(DescribePackageRemovalPreviewCommand().options.front());
        descriptor.output.id = "horo.package.remove";
        return descriptor;
    }

    /** @copydoc MakePackageRemovalPreviewCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePackageRemovalPreviewCommandAdapter() {
        return {.adapter = std::make_unique<PackageUpdatePreviewAdapter>(DescribePackageRemovalPreviewCommand(), PackageOperation::Removal),
                .capabilities = {{"horo.package.manage"}}};
    }

    /** @copydoc MakePackageApplyRemovalCommandAdapter */
    Cli::CliCommandAdapterRegistration MakePackageApplyRemovalCommandAdapter() {
        return {.adapter = std::make_unique<PackageApplyUpdateAdapter>(DescribePackageApplyRemovalCommand(), PackageOperation::Removal),
                .capabilities = {{"horo.package.manage"}}};
    }

    /** @copydoc PackageCliHost::PackageCliHost */
    PackageCliHost::PackageCliHost() = default;

    /** @copydoc PackageCliHost::~PackageCliHost */
    PackageCliHost::~PackageCliHost() noexcept {
        // Keep every borrowed service alive while cancellation joins source/install/metadata callbacks.
        try {
            if (project_)
                static_cast<void>(project_->Close());
        } catch (...) {
            // This fallback still retains the owner until scheduler cancellation joins borrowed callbacks.
            std::fputs("Package close raised before scheduler cancellation; retaining the owner.\n", stderr);
        }
        jobs_.Shutdown(ShutdownPolicy::Cancel);
        if (project_) {
            try {
                if (project_->Close().HasError())
                    std::terminate();
            } catch (...) {
                std::terminate();
            }
        }
    }

    /** @copydoc PackageCliHost::Bind */
    Result<std::shared_ptr<PackageApplication>> PackageCliHost::Bind(const Cli::CliCommandRequest &request,
                                                                     const Cli::CliExecutionContext &context) {
        auto root = PathOption(request, "project");
        if (root.HasError())
            return Result<std::shared_ptr<PackageApplication>>::Failure(root.ErrorValue());
        auto cache = PathOption(request, "package-cache-root");
        if (cache.HasError())
            return Result<std::shared_ptr<PackageApplication>>::Failure(cache.ErrorValue());
        auto temporary = PathOption(request, "package-temporary-root");
        if (temporary.HasError())
            return Result<std::shared_ptr<PackageApplication>>::Failure(temporary.ErrorValue());
        if (!project_) {
            auto platform = NativePackagePlatform();
            if (platform.HasError())
                return Result<std::shared_ptr<PackageApplication>>::Failure(platform.ErrorValue());
            PackageProjectPolicy policy{.cacheRoot = cache.Value(),
                                        .platform = std::move(platform).Value(),
                                        .lifecycle = {.temporaryRoot = temporary.Value(),
                                                      .profile = Extensions::ExtensionHostProfile::Headless,
                                                      .artifactGate = std::make_shared<UnconfiguredPackageGate>()}};
            auto created = PackageProjectHost::Create({jobs_, files_, clock_, wallClock_, trust_}, std::move(policy),
                                                      Duration::FromMilliseconds(1'000));
            if (created.HasError())
                return Result<std::shared_ptr<PackageApplication>>::Failure(created.ErrorValue());
            project_ = std::move(created).Value();
            projectRoot_ = root.Value();
            cacheRoot_ = cache.Value();
            temporaryRoot_ = temporary.Value();
        } else if (root.Value() != projectRoot_ || cache.Value() != cacheRoot_ || temporary.Value() != temporaryRoot_)
            return Result<std::shared_ptr<PackageApplication>>::Failure(MakeError(Cli::CliErrors::ExecutionContextInvalid));
        while (!context.IsStopRequested()) {
            if (auto acquired = project_->Acquire(projectRoot_); acquired.HasValue() || !Preparing(acquired.ErrorValue()))
                return acquired;
            // Tooling-only owner polling; deadline/cancellation remain those of the real dispatcher invocation.
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        static_cast<void>(project_->Close());
        const auto remaining = context.RemainingTimeout();
        return Result<std::shared_ptr<PackageApplication>>::Failure(
            MakeError(remaining && remaining->count() == 0 ? Cli::CliErrors::ExecutionTimedOut : Cli::CliErrors::ExecutionCancelled));
    }

    /** @copydoc PackageCliHost::BindMcp */
    Result<McpPackageBinding> PackageCliHost::BindMcp(const Cli::CliCommandRequest &request, const Cli::CliExecutionContext &context) {
        auto application = Bind(request, context);
        if (application.HasError())
            return Result<McpPackageBinding>::Failure(application.ErrorValue());
        const auto identity = ProjectProtocolIdentity(projectRoot_);
        return Result<McpPackageBinding>::Success({.application = std::move(application).Value(),
                                                   .projectIdentity = identity,
                                                   .generation = project_->Generation(),
                                                   .currentGeneration =
                                                       [this] {
            return project_->Generation();
        },
                                                   .pump = [this] {
            return project_->Pump(Packages::PackageActivationBoundary::Quiescent);
        }});
    }

    /** @copydoc PackageCliHost::Boundary */
    Result<Packages::PackageActivationBoundary> PackageCliHost::Boundary() {
        if (!project_)
            return Result<Packages::PackageActivationBoundary>::Failure(MakeError(Cli::CliErrors::CommandUnavailable));
        if (auto pumped = project_->Pump(Packages::PackageActivationBoundary::Quiescent); pumped.HasError())
            return Result<Packages::PackageActivationBoundary>::Failure(pumped.ErrorValue());
        return Result<Packages::PackageActivationBoundary>::Success(Packages::PackageActivationBoundary::Quiescent);
    }

    /** @copydoc PackageCliHost::Project */
    PackageProjectHost *PackageCliHost::Project() noexcept {
        return project_.get();
    }
}  // namespace Horo::Application::Internal
