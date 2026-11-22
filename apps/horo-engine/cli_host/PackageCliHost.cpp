#include "PackageCliHost.h"

#include "Horo/Cli/CliErrors.h"
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
        Cli::CliCommandResult EncodePreview(const Packages::PackageUpdatePreview &preview, const std::size_t offset) {
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
                               {"changes", std::move(rows)}}};
        }

        /** @brief Compares bounded candidate documents to the live, read-only project metadata snapshot. */
        Result<Packages::PackageUpdatePreview> InspectUpdate(const Cli::CliCommandRequest &request, const CancellationToken &cancellation) {
            using Output = Result<Packages::PackageUpdatePreview>;
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
            const PackageProjectPolicy policy{.projectRoot = root.Value(), .platform = std::move(platform).Value()};
            NativeDurableFileSystem files;
            const auto candidateIntent = files.ReadFileBounded(requestPath.Value(), 1024 * 1024, cancellation);
            if (candidateIntent.HasError())
                return Output::Failure(candidateIntent.ErrorValue());
            const auto candidate =
                Packages::ValidatedPackageRequest::Parse(std::string{candidateIntent.Value().begin(), candidateIntent.Value().end()});
            if (candidate.HasError())
                return Output::Failure(candidate.ErrorValue());
            const auto lockBytes = files.ReadFileBounded(lockPath.Value(), policy.limits.lockfile.documentBytes, cancellation);
            if (lockBytes.HasError())
                return Output::Failure(lockBytes.ErrorValue());
            const auto lock = Packages::ValidatedPackageLockfileV1::Parse(std::string{lockBytes.Value().begin(), lockBytes.Value().end()},
                                                                          policy.limits.lockfile);
            if (lock.HasError())
                return Output::Failure(lock.ErrorValue());
            if (const auto supported = lock.Value().ValidateForRestore(candidate.Value().Digest(), policy.platform,
                                                                       policy.limits.supportedPackageFormatVersion);
                supported.HasError())
                return Output::Failure(supported.ErrorValue());
            const auto prepared = PreparePackageProject(policy, files, cancellation, PackageProjectReadMode::ReadOnly);
            if (prepared.HasError())
                return Output::Failure(prepared.ErrorValue());
            const auto &base = prepared.Value().application;
            return Packages::PackageUpdatePreview::Prepare(*base.portableRequest, *base.lockedPlan, candidate.Value(), lock.Value());
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
