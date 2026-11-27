#include "PackageCliHost.h"

#include "Horo/Application/ProjectAssetRegistrySource.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "PackageBehaviorReferences.h"
#include "PackageProjectSession.h"
#include "PackageUpdateReferences.h"
#include "ProjectProtocolIdentity.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <iterator>
#include <ranges>
#include <set>
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

            /** @brief Identifies updates that must recheck saved references to departing packages. */
            bool HasWithdrawals() const {
                return std::ranges::any_of(preview.changes, [](const auto &change) {
                    return change.current && !change.candidate;
                });
            }

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

        /** @brief Reports saved importer references and lock-level blockers before issuing an offline approval. */
        Result<void> ReviewOfflineRemoval(const InspectedPackageUpdate &update, const Cli::CliCommandRequest &request,
                                          const Cli::CliExecutionContext &context);
        /** @brief Checks the verified archives if the caller requested a reference-aware preview. */
        Result<void> AppendBehaviorMigrationDiagnostics(const PackageProjectPolicy &policy, const Cli::CliExecutionContext &context,
                                                         std::span<const PackageBehaviorMigration> claims, std::vector<Diagnostic> &diagnostics);

        Result<void> ReviewCachedUpdate(const InspectedPackageUpdate &update, const Cli::CliCommandRequest &request,
                                        const Cli::CliExecutionContext &context);

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
                const auto &update = preview.Value();
                const auto review =
                    update.HasWithdrawals() ? ReviewOfflineRemoval(update, request, context) : ReviewCachedUpdate(update, request, context);
                if (review.HasError())
                    return Output::Failure(review.ErrorValue());
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

        /** @brief Checks unchanged entries required by offline staging without re-reading changed archives. */
        Result<void> ReviewCachedRetainedEntries(const InspectedPackageUpdate &update, const Packages::PackageCacheStore &cache,
                                                 const Cli::CliExecutionContext &context, std::vector<Diagnostic> &diagnostics,
                                                 const std::size_t diagnosticLimit) {
            std::set<Packages::HoroPackageId> changed;
            for (const auto &change : update.preview.changes)
                if (change.current)
                    changed.insert(change.current->package);
            for (const auto &entry : update.base.application.lockedPlan->Packages()) {
                if (context.IsStopRequested())
                    return Result<void>::Failure(UpdateStopped(context));
                if (diagnostics.size() >= diagnosticLimit)
                    break;
                if (changed.contains(entry.package))
                    continue;
                if (const auto verified = ReadUpdateArchive(cache, entry); verified.HasError())
                    diagnostics.emplace_back(DiagnosticCode{"packages.host.update_archive_evidence_unavailable"}, DiagnosticSeverity::Error,
                                             verified.ErrorValue().code.Value(), SourceLocation{"packages.lock"}, entry.package.Value());
            }
            return Result<void>::Success();
        }

        Result<void> ReviewCachedUpdate(const InspectedPackageUpdate &update, const Cli::CliCommandRequest &request,
                                        const Cli::CliExecutionContext &context) {
            if (std::ranges::find(request.options, "package-cache-root", &Cli::CliParsedOption::name) == request.options.end())
                return Result<void>::Success();
            const auto root = PathOption(request, "package-cache-root");
            if (root.HasError())
                return Result<void>::Failure(root.ErrorValue());
            NativeDurableFileSystem files;
            const auto cache = Packages::PackageCacheStore::Create(files, root.Value(), update.policy.limits.validation);
            if (cache.HasError())
                return Result<void>::Failure(cache.ErrorValue());
            auto diagnostics = ReviewUpdateLock(update.preview);
            std::vector<PackageBehaviorMigration> claims;
            std::optional<Error> firstFailure;
            for (const auto &change : update.preview.changes) {
                if (context.IsStopRequested())
                    return Result<void>::Failure(UpdateStopped(context));
                if (diagnostics.size() >= MaximumUpdateDiagnostics)
                    break;
                ReviewCachedChange(change, cache.Value(), diagnostics, claims, firstFailure);
            }
            if (auto retained = ReviewCachedRetainedEntries(update, cache.Value(), context, diagnostics, MaximumUpdateDiagnostics);
                retained.HasError())
                return retained;
            if (auto reviewed = AppendBehaviorMigrationDiagnostics(update.policy, context, claims, diagnostics); reviewed.HasError())
                return reviewed;
            if (context.IsStopRequested())
                return Result<void>::Failure(UpdateStopped(context));
            if (firstFailure) {
                std::ranges::move(diagnostics, std::back_inserter(firstFailure->diagnostics));
                return Result<void>::Failure(std::move(*firstFailure));
            }
            return diagnostics.empty() ? Result<void>::Success() : Result<void>::Failure(UpdateNeedsMigration(std::move(diagnostics)));
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

        /** @brief Retains the same root-bound assets and file authority for preview or an approved mutation. */
        struct RemovalInventory final {
            Platform::ProjectReadContext admission;
            std::shared_ptr<const Platform::IProjectReadFiles> files;
            std::shared_ptr<const Assets::IAssetRegistrySource> assets;
        };

        /** @brief Opens the project inventory with the exact command's timeout and cancellation. */
        Result<RemovalInventory> OpenRemovalInventory(const PackageProjectPolicy &policy, const Cli::CliExecutionContext &context) {
            using Inventory = Result<RemovalInventory>;
            auto admission = ReadAuthority(policy, context);
            auto read = Platform::CreateNativeProjectReadFiles(policy.projectRoot, admission.projectIdentity, admission.projectGeneration);
            if (read.HasError())
                return Inventory::Failure(read.ErrorValue());
            auto assets = CreateProjectAssetRegistrySource(read.Value(), admission);
            if (assets.HasError())
                return Inventory::Failure(assets.ErrorValue());
            return Inventory::Success({std::move(admission), std::move(read).Value(), std::move(assets).Value()});
        }

        /** @brief Resolves possible attachment impacts in the bounded saved asset inventory without activating either artifact. */
        Result<void> AppendBehaviorMigrationDiagnostics(const PackageProjectPolicy &policy, const Cli::CliExecutionContext &context,
                                                         std::span<const PackageBehaviorMigration> claims, std::vector<Diagnostic> &diagnostics) {
            if (claims.empty())
                return Result<void>::Success();
            const auto inventory = OpenRemovalInventory(policy, context);
            if (inventory.HasError())
                return Result<void>::Failure(WithCause(UpdateNeedsMigration(std::move(diagnostics)), inventory.ErrorValue()));
            const auto &inputs = inventory.Value();
            const auto remaining = MaximumUpdateDiagnostics - diagnostics.size();
            auto matches = InspectPackageBehaviorMigrations(*inputs.assets, *inputs.files, inputs.admission, claims, remaining);
            if (matches.HasError()) {
                auto failure = std::move(matches).ErrorValue();
                std::ranges::move(failure.diagnostics | std::views::take(remaining), std::back_inserter(diagnostics));
                failure.diagnostics.clear();
                return Result<void>::Failure(WithCause(UpdateNeedsMigration(std::move(diagnostics)), std::move(failure)));
            }
            auto notes = std::move(matches).Value();
            std::ranges::move(notes, std::back_inserter(diagnostics));
            return Result<void>::Success();
        }

        /** @brief Reads the saved importer inventory through the same filesystem authority used during removal. */
        Result<std::vector<PackageImporterReference>> ReadRemovalImporters(const PackageProjectPolicy &policy,
                                                                            const Packages::PackageRemovalGraphPreview &impact,
                                                                            const Cli::CliExecutionContext &context) {
            const auto inventory = OpenRemovalInventory(policy, context);
            if (inventory.HasError())
                return Result<std::vector<PackageImporterReference>>::Failure(inventory.ErrorValue());
            return InspectPackageRemovalImporters(*inventory.Value().assets, impact);
        }

        /** @brief Checks optional sealed cache evidence without restoring packages or activating executable code. */
        Result<void> ReviewCachedRemoval(const InspectedPackageUpdate &update, const Cli::CliCommandRequest &request,
                                         const Cli::CliExecutionContext &context, std::vector<Diagnostic> &diagnostics) {
            if (std::ranges::find(request.options, "package-cache-root", &Cli::CliParsedOption::name) == request.options.end())
                return Result<void>::Success();
            const auto root = PathOption(request, "package-cache-root");
            if (root.HasError())
                return Result<void>::Failure(root.ErrorValue());
            NativeDurableFileSystem files;
            const auto cache = Packages::PackageCacheStore::Create(files, root.Value(), update.policy.limits.validation);
            if (cache.HasError())
                return Result<void>::Failure(cache.ErrorValue());
            for (const auto &change : update.preview.changes) {
                if (context.IsStopRequested())
                    return Result<void>::Failure(UpdateStopped(context));
                const auto &entry = change.current ? *change.current : *change.candidate;
                const auto archive = ReadUpdateArchive(cache.Value(), entry);
                if (archive.HasError()) {
                    const auto *code = change.candidate ? "packages.host.update_archive_evidence_unavailable"
                                                       : "packages.host.removal_archive_evidence_unavailable";
                    diagnostics.emplace_back(DiagnosticCode{code}, DiagnosticSeverity::Error, archive.ErrorValue().code.Value(),
                                             SourceLocation{"packages.host"}, entry.package.Value());
                } else if (!change.current) {
                    if (auto reviewed = ReviewAddedArchive(entry, archive.Value(), diagnostics); reviewed.HasError())
                        return reviewed;
                } else if (change.candidate) {
                    const auto candidate = ReadUpdateArchive(cache.Value(), *change.candidate);
                    if (candidate.HasError()) {
                        diagnostics.emplace_back(DiagnosticCode{"packages.host.update_archive_evidence_unavailable"}, DiagnosticSeverity::Error,
                                                 candidate.ErrorValue().code.Value(), SourceLocation{"packages.lock"},
                                                 change.candidate->package.Value());
                    } else if (auto reviewed = ReviewAssetFiles(*change.current, archive.Value(), *change.candidate, candidate.Value(), diagnostics);
                               reviewed.HasError()) {
                        return reviewed;
                    }
                } else {
                    if (auto reviewed = AppendOfflineRemovalArchiveBlockers(diagnostics, *change.current, archive.Value()); reviewed.HasError())
                        return reviewed;
                }
                if (diagnostics.size() > MaximumRemovalCoverageDiagnostics)
                    return Result<void>::Failure(
                        Error{ErrorCode{"packages.host.removal_reference_coverage_limit"}, ErrorDomainId{"packages.host"}});
            }
            return ReviewCachedRetainedEntries(update, cache.Value(), context, diagnostics, MaximumRemovalCoverageDiagnostics);
        }

        Result<void> ReviewOfflineRemoval(const InspectedPackageUpdate &update, const Cli::CliCommandRequest &request,
                                          const Cli::CliExecutionContext &context) {
            if (update.operation == PackageOperation::Forward &&
                std::ranges::find(request.options, "package-cache-root", &Cli::CliParsedOption::name) == request.options.end())
                return Result<void>::Success();
            Packages::PackageRemovalGraphPreview impact;
            std::vector<Diagnostic> diagnostics;
            for (const auto &change : update.preview.changes) {
                if (change.candidate &&
                    (!IsAssetOnly(*change.candidate) || (change.current && !CanRestoreRetainedAsset(*change.current, *change.candidate))))
                    return Result<void>::Failure(UpdateNeedsMigration());
                if (change.candidate)
                    continue;
                impact.removed.push_back({change.current->package, change.current->version});
                if (auto blocker = OfflineRemovalContributionBlocker(*change.current))
                    diagnostics.push_back(std::move(*blocker));
            }
            if (impact.removed.empty())
                return Result<void>::Success();
            auto review = ReviewCachedRemoval(update, request, context, diagnostics);
            if (context.IsStopRequested())
                return Result<void>::Failure(UpdateStopped(context));
            auto references = ReadRemovalImporters(update.policy, impact, context);
            if (references.HasValue())
                AppendPackageImporterDiagnostics(diagnostics, references.Value());
            if (review.HasError()) {
                auto failure = std::move(review).ErrorValue();
                if (references.HasError()) {
                    auto referenceFailure = std::move(references).ErrorValue();
                    diagnostics.emplace_back(DiagnosticCode{referenceFailure.code.Value()}, DiagnosticSeverity::Error,
                                             std::move(referenceFailure.message), SourceLocation{referenceFailure.domain.Value()}, "");
                    std::ranges::move(referenceFailure.diagnostics, std::back_inserter(diagnostics));
                }
                std::ranges::move(diagnostics, std::back_inserter(failure.diagnostics));
                return Result<void>::Failure(std::move(failure));
            }
            if (references.HasError()) {
                auto failure = std::move(references).ErrorValue();
                std::ranges::move(diagnostics, std::back_inserter(failure.diagnostics));
                return Result<void>::Failure(std::move(failure));
            }
            if (diagnostics.empty())
                return Result<void>::Success();
            return Result<void>::Failure(Error{ErrorCode{diagnostics.front().code.Value()}, ErrorDomainId{"packages.host"},
                                               ErrorSeverity::Error, "", std::move(diagnostics)});
        }

        /** @brief Retains the verified departing graph and fresh saved-project evidence through publication. */
        struct OfflineWithdrawalEvidence final {
            Packages::StagedPackageRestoreRecord staged;
            RemovalInventory inventory;

            /** @brief Borrows the staged archive and project authorities from this evidence owner. */
            PackageRemovalSources Sources() const {
                return {.assets = *inventory.assets, .files = *inventory.files, .context = inventory.admission, .offline = &staged};
            }
        };

        /** @brief Opens the same current-lock evidence for uninstall and rollback. */
        Result<OfflineWithdrawalEvidence> PrepareOfflineWithdrawal(const InspectedPackageUpdate &update, NativeDurableFileSystem &files,
                                                                   Packages::PackageCacheStore &cache,
                                                                   const Cli::CliExecutionContext &context) {
            using Output = Result<OfflineWithdrawalEvidence>;
            auto restore = update.base.application.restore;
            restore.cancellation = context.Cancellation();
            auto verified = StageWithCache(restore, files, cache, context);
            if (verified.HasError())
                return Output::Failure(std::move(verified).ErrorValue());
            auto inventory = OpenRemovalInventory(update.policy, context);
            if (inventory.HasError())
                return Output::Failure(std::move(inventory).ErrorValue());
            return Output::Success({std::move(verified).Value(), std::move(inventory).Value()});
        }

        /** @brief Publishes an approved offline withdrawal after inspecting the real root-bound project inventory. */
        Result<Cli::CliCommandResult> ApplyRemoval(const InspectedPackageUpdate &update, NativeDurableFileSystem &files,
                                                   Packages::PackageCacheStore &cache, const Cli::CliExecutionContext &context) {
            using Output = Result<Cli::CliCommandResult>;
            const auto evidence = PrepareOfflineWithdrawal(update, files, cache, context);
            if (evidence.HasError())
                return Output::Failure(evidence.ErrorValue());
            const auto published =
                PublishPackageProjectRemoval(update.policy, update.base, *update.removalPackage, files, evidence.Value().Sources());
            if (published.HasError())
                return Output::Failure(published.ErrorValue());
            return Output::Success({.fields = {{"recoveryRequired", published.Value().recoveryDiagnostic.has_value()}}});
        }

        /** @brief Checks the exact staged archives and any previous asset bytes before publication. */
        Result<void> CheckStagedUpdateArchives(const InspectedPackageUpdate &update, const Packages::PackageRestoreGraph &staged,
                                                const Packages::PackageCacheStore &cache, const Cli::CliExecutionContext &context) {
            std::vector<Diagnostic> diagnostics;
            const auto blocked = [&diagnostics](Error error) {
                std::ranges::move(diagnostics, std::back_inserter(error.diagnostics));
                return Result<void>::Failure(std::move(error));
            };
            for (const auto &change : update.preview.changes) {
                if (context.IsStopRequested())
                    return Result<void>::Failure(UpdateStopped(context));
                if (!change.candidate)
                    return Result<void>::Failure(UpdateNeedsMigration());
                const auto next = std::ranges::find(staged.packages, *change.candidate, &Packages::PackageRestorePackage::lock);
                if (next == staged.packages.end() || !next->archive)
                    return Result<void>::Failure(UpdateNeedsMigration());
                if (!change.current) {
                    if (auto reviewed = ReviewAddedArchive(next->lock, *next->archive, diagnostics); reviewed.HasError())
                        return blocked(std::move(reviewed).ErrorValue());
                    continue;
                }
                const auto &old = *change.current;
                const auto previous = ReadUpdateArchive(cache, old);
                if (previous.HasError())
                    return Result<void>::Failure(previous.ErrorValue());
                if (auto reviewed = ReviewAssetFiles(old, previous.Value(), next->lock, *next->archive, diagnostics); reviewed.HasError())
                    return blocked(std::move(reviewed).ErrorValue());
            }
            return diagnostics.empty() ? Result<void>::Success() : Result<void>::Failure(UpdateNeedsMigration(std::move(diagnostics)));
        }

        /** @brief Stages the departing lock and supplies fresh project evidence to the update transaction. */
        Result<PackageMetadataPublication> PublishWithWithdrawals(const InspectedPackageUpdate &update,
                                                                  const ApprovedPackageProjectUpdate &approved,
                                                                  NativeDurableFileSystem &files, Packages::PackageCacheStore &cache,
                                                                  const Cli::CliExecutionContext &context) {
            const auto evidence = PrepareOfflineWithdrawal(update, files, cache, context);
            if (evidence.HasError())
                return Result<PackageMetadataPublication>::Failure(evidence.ErrorValue());
            return PublishPackageUpdateWithWithdrawals(update.policy, approved, files, evidence.Value().Sources());
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
            const bool withdrawal = update.HasWithdrawals();
            if (!withdrawal) {
                if (const auto content = CheckStagedUpdateArchives(update, *staged.Value().Graph(), cache, context); content.HasError())
                    return Output::Failure(content.ErrorValue());
            }

            const ApprovedPackageProjectUpdate approved{update.base, staged.Value(), update.approval, update.request, update.lock};
            const auto published = withdrawal
                                       ? PublishWithWithdrawals(update, approved, files, cache, context)
                                       : PublishPackageProjectUpdate(update.policy, approved, files, ReadAuthority(update.policy, context));
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
            if (!update.HasWithdrawals()) {
                auto blockers = ReviewUpdateLock(update.preview);
                if (!blockers.empty())
                    return Output::Failure(UpdateNeedsMigration(std::move(blockers)));
            }
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
                             .numericRange = {.minimumInteger = 0, .maximumInteger = 1024}},
                            {.name = "package-cache-root",
                             .summary = "Optional absolute verified cache root for a read-only migration review.",
                             .valueKind = CliOptionValueKind::Path}},
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
        std::erase_if(descriptor.options, [](const auto &option) {
            return option.name == "offset" || option.name == "package-cache-root";
        });
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

    /** @brief Shares the directly requested package selector between preview and apply. */
    static Cli::CliOptionDescriptor PackageIdOption() {
        return {.name = "package-id",
                .summary = "Portable ID of a directly requested package.",
                .valueKind = Cli::CliOptionValueKind::String,
                .required = true};
    }

    /** @copydoc DescribePackageRemovalPreviewCommand */
    Cli::CliCommandDescriptor DescribePackageRemovalPreviewCommand() {
        auto descriptor = DescribePackageRollbackPreviewCommand();
        descriptor.path = {{"package", "preview-remove"}};
        descriptor.summary = "Review the exact package withdrawal and its approval fingerprint.";
        std::ranges::find(descriptor.options, "package-cache-root", &Cli::CliOptionDescriptor::name)->summary =
            "Optional absolute cache root for verifying the exact departing archives.";
        descriptor.options.push_back(PackageIdOption());
        descriptor.output.id = "horo.package.preview-remove";
        return descriptor;
    }

    /** @copydoc DescribePackageApplyRemovalCommand */
    Cli::CliCommandDescriptor DescribePackageApplyRemovalCommand() {
        auto descriptor = DescribePackageRollbackUpdateCommand();
        descriptor.path = {{"package", "remove"}};
        descriptor.summary = "Withdraw an approved asset package from a local project.";
        descriptor.options.push_back(PackageIdOption());
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
