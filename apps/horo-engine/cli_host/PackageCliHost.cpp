#include "PackageCliHost.h"

#include "Horo/Cli/CliErrors.h"
#include "Horo/Platform/ProjectReadFiles.h"
#include "PackageProjectSession.h"
#include "ProjectProtocolIdentity.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
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

        struct InspectedPackageUpdate final {
            PackageProjectPolicy policy;
            PreparedPackageProject base;
            PackageUpdateApproval approval;
            std::string request;
            std::string lock;
            Sha256Digest requestHash;
            Packages::PackageUpdatePreview preview;

            /** @brief Identifies the reviewed project and exact old and proposed documents. */
            std::string ApprovalFingerprint() const {
                const auto bytes = std::format("horo.package.update.v1 {} {} {} {} {}", ProjectProtocolIdentity(policy.projectRoot),
                                               FormatSha256(approval.baseRequest), FormatSha256(approval.baseLock),
                                               FormatSha256(approval.candidateRequest), FormatSha256(approval.candidateLock));
                return FormatSha256(ComputeSha256(std::as_bytes(std::span{bytes})));
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

        /** @brief Compares bounded candidate documents to the live, read-only project metadata snapshot. */
        Result<InspectedPackageUpdate> InspectUpdate(const Cli::CliCommandRequest &request, const CancellationToken &cancellation) {
            using Output = Result<InspectedPackageUpdate>;
            const auto root = PathOption(request, "project");
            if (root.HasError())
                return Output::Failure(root.ErrorValue());
            const auto requestPath = PathOption(request, "candidate-request");
            if (requestPath.HasError())
                return Output::Failure(requestPath.ErrorValue());
            const auto lockPath = PathOption(request, "candidate-lock");
            if (lockPath.HasError())
                return Output::Failure(lockPath.ErrorValue());
            auto platform = NativePackagePlatform();
            if (platform.HasError())
                return Output::Failure(platform.ErrorValue());
            PackageProjectPolicy policy{.projectRoot = root.Value(), .platform = std::move(platform).Value()};
            NativeDurableFileSystem files;
            const auto candidateIntent = files.ReadFileBounded(requestPath.Value(), 1024 * 1024, cancellation);
            if (candidateIntent.HasError())
                return Output::Failure(candidateIntent.ErrorValue());
            std::string candidateRequest{candidateIntent.Value().begin(), candidateIntent.Value().end()};
            const auto candidate = Packages::ValidatedPackageRequest::Parse(candidateRequest);
            if (candidate.HasError())
                return Output::Failure(candidate.ErrorValue());
            const auto lockBytes = files.ReadFileBounded(lockPath.Value(), policy.limits.lockfile.documentBytes, cancellation);
            if (lockBytes.HasError())
                return Output::Failure(lockBytes.ErrorValue());
            std::string candidateLock{lockBytes.Value().begin(), lockBytes.Value().end()};
            const auto lock = Packages::ValidatedPackageLockfileV1::Parse(candidateLock, policy.limits.lockfile);
            if (lock.HasError())
                return Output::Failure(lock.ErrorValue());
            if (const auto supported = lock.Value().ValidateForRestore(candidate.Value().Digest(), policy.platform,
                                                                       policy.limits.supportedPackageFormatVersion);
                supported.HasError())
                return Output::Failure(supported.ErrorValue());
            auto prepared = PreparePackageProject(policy, files, cancellation, PackageProjectReadMode::ReadOnly);
            if (prepared.HasError())
                return Output::Failure(prepared.ErrorValue());
            const auto &base = prepared.Value().application;
            auto preview =
                Packages::PackageUpdatePreview::Prepare(*base.portableRequest, *base.lockedPlan, candidate.Value(), lock.Value());
            if (preview.HasError())
                return Output::Failure(preview.ErrorValue());
            const PackageUpdateApproval approval{prepared.Value().requestDocumentDigest, prepared.Value().lockDigest,
                                                 ComputeSha256(std::as_bytes(std::span{candidateRequest})),
                                                 ComputeSha256(std::as_bytes(std::span{candidateLock}))};
            return Output::Success({std::move(policy), std::move(prepared).Value(), approval, std::move(candidateRequest),
                                    std::move(candidateLock), candidate.Value().Digest(), std::move(preview).Value()});
        }

        class PackageUpdatePreviewAdapter final : public Cli::ICliCommandAdapter {
        public:
            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<Cli::CliCommandResult> Execute(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &context) override {
                using Output = Result<Cli::CliCommandResult>;
                const auto offset = PreviewOffset(request);
                if (offset.HasError())
                    return Output::Failure(offset.ErrorValue());
                const auto preview = InspectUpdate(request, context.Cancellation());
                if (preview.HasError())
                    return Output::Failure(preview.ErrorValue());
                return Output::Success(EncodePreview(preview.Value(), offset.Value()));
            }

        private:
            Cli::CliCommandDescriptor descriptor_{DescribePackageUpdatePreviewCommand()};
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
        Result<Packages::StagedPackageRestoreRecord> StageUpdate(const InspectedPackageUpdate &update,
                                                                 Packages::PackageRestoreService &restore,
                                                                 const Cli::CliExecutionContext &context) {
            using Output = Result<Packages::StagedPackageRestoreRecord>;
            const auto started = restore.StartStaged({.projectRoot = update.policy.projectRoot,
                                                      .lockfileJson = update.lock,
                                                      .expectedRequestHash = update.requestHash,
                                                      .platform = update.policy.platform,
                                                      .mode = update.policy.mode,
                                                      .cancellation = context.Cancellation()});
            if (started.HasError())
                return Output::Failure(started.ErrorValue());
            const auto operation = started.Value().Id();
            while (!context.IsStopRequested()) {
                const auto progress = restore.Query(operation);
                if (!progress)
                    break;
                if (progress->outcome == Packages::PackageRestoreOutcome::Running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    continue;
                }
                if (auto staged = restore.Staged(operation))
                    return Output::Success(std::move(*staged));
                if (progress->diagnostic)
                    return Output::Failure(*progress->diagnostic);
                break;
            }
            if (context.IsStopRequested())
                return Output::Failure(UpdateStopped(context));
            return Output::Failure(Error{ErrorCode{"packages.host.staging_unavailable"}, ErrorDomainId{"packages.host"}});
        }

        /** @brief Applies an approved update that preserves every existing locked package. */
        Result<Cli::CliCommandResult> ApplyUpdate(const Cli::CliCommandRequest &request, const InspectedPackageUpdate &update,
                                                  const Cli::CliExecutionContext &context) {
            using Output = Result<Cli::CliCommandResult>;
            if (UpdateApproval(request) != update.ApprovalFingerprint())
                return Output::Failure(Error{ErrorCode{"packages.host.update_approval_mismatch"}, ErrorDomainId{"packages.host"}});
            if (std::ranges::any_of(update.preview.changes, [](const auto &change) {
                return change.current.has_value();
            }))
                return Output::Failure(Error{ErrorCode{"packages.host.update_requires_migration_review"}, ErrorDomainId{"packages.host"}});
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
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto restore = Packages::PackageRestoreService::Create(jobs, files, verifiedCache);
            if (restore.HasError())
                return Output::Failure(restore.ErrorValue());
            auto service = std::move(restore).Value();
            const auto staged = StageUpdate(update, service, context);
            if (staged.HasError())
                return Output::Failure(staged.ErrorValue());

            const auto remaining = context.RemainingTimeout();
            const Platform::ProjectReadContext admission{.projectIdentity = ProjectProtocolIdentity(update.policy.projectRoot),
                                                         .projectGeneration = 1,
                                                         .cancellation = context.Cancellation(),
                                                         .deadline = remaining ? std::chrono::steady_clock::now() + *remaining
                                                                               : std::chrono::steady_clock::time_point::max(),
                                                         .authorityStopped = [&context] {
                return context.IsStopRequested();
            }};
            const ApprovedPackageProjectUpdate approved{update.base, staged.Value(), update.approval, update.request, update.lock};
            const auto published = PublishPackageProjectUpdate(update.policy, approved, files, admission);
            if (published.HasError())
                return Output::Failure(published.ErrorValue());
            return Output::Success({.fields = {{"recoveryRequired", published.Value().recoveryDiagnostic.has_value()}}});
        }

        class PackageApplyUpdateAdapter final : public Cli::ICliCommandAdapter {
        public:
            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<Cli::CliCommandResult> Execute(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &context) override {
                const auto update = InspectUpdate(request, context.Cancellation());
                if (update.HasError())
                    return Result<Cli::CliCommandResult>::Failure(update.ErrorValue());
                return ApplyUpdate(request, update.Value(), context);
            }

        private:
            Cli::CliCommandDescriptor descriptor_{DescribePackageApplyUpdateCommand()};
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
        return {.adapter = std::make_unique<PackageUpdatePreviewAdapter>(), .capabilities = {{"horo.package.manage"}}};
    }

    /** @copydoc DescribePackageApplyUpdateCommand */
    Cli::CliCommandDescriptor DescribePackageApplyUpdateCommand() {
        using namespace Cli;
        auto descriptor = DescribePackageUpdatePreviewCommand();
        descriptor.path = {{"package", "apply-update"}};
        descriptor.summary = "Apply an approved offline update that preserves the existing locked packages.";
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
        return {.adapter = std::make_unique<PackageApplyUpdateAdapter>(), .capabilities = {{"horo.package.manage"}}};
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
