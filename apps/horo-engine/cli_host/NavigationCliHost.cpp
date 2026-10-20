#include "NavigationCliHost.h"

#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "Horo/Platform/SecureRandom.h"
#include "ProjectProtocolIdentity.h"

#include <algorithm>
#include <array>

namespace Horo::Application::Internal {
    namespace {
        namespace Nav = Navigation;

        /** @brief Reads exactly one parser-owned option, without reparsing argv or consulting ambient state. */
        const Cli::CliParsedValue *Option(const Cli::CliCommandRequest &request, std::string_view name) {
            const auto found = std::ranges::find(request.options, name, &Cli::CliParsedOption::name);
            return found != request.options.end() && found->values.size() == 1 ? &found->values.front().value : nullptr;
        }

        /** @brief Resolves only explicit absolute normalized project input; output paths are fixed descendants. */
        Result<std::filesystem::path> Root(const Cli::CliCommandRequest &request) {
            const auto *value = Option(request, "project");
            const auto *path = value ? std::get_if<Cli::CliPathValue>(value) : nullptr;
            if (!path)
                return Result<std::filesystem::path>::Failure(MakeError(Cli::CliErrors::ParseFailed));
            const auto input = std::filesystem::path{std::u8string{path->value.begin(), path->value.end()}};
            if (!input.is_absolute() || input != input.lexically_normal())
                return Result<std::filesystem::path>::Failure(MakeError(Cli::CliErrors::ParseFailed));
            std::error_code error;
            auto root = std::filesystem::canonical(input, error);
            if (error)
                return Result<std::filesystem::path>::Failure(MakeError(Platform::ProjectReadErrors::ReadFailed));
            return Result<std::filesystem::path>::Success(std::move(root));
        }

        /** @brief Refuses redirected existing output ancestors before the existing durable writer can create missing descendants. */
        Result<std::filesystem::path> Output(const std::filesystem::path &root, const std::filesystem::path &relative) {
            const auto path = root / relative;
            std::error_code error;
            if (const auto resolved = std::filesystem::weakly_canonical(path, error); error || resolved != path)
                return Result<std::filesystem::path>::Failure(MakeError(Platform::ProjectReadErrors::UnsafePath));
            return Result<std::filesystem::path>::Success(path);
        }

        /** @brief Generates only private operation namespace UUIDs from native entropy, never authored asset identities. */
        Result<Assets::AssetId> NewOperation() {
            std::array<std::byte, 16> bytes{};
            auto entropy = Platform::CreateNativeSecureRandomSource();
            if (auto filled = entropy->Fill(bytes); filled.HasError())
                return Result<Assets::AssetId>::Failure(filled.ErrorValue());
            bytes[6] = (bytes[6] & std::byte{0x0f}) | std::byte{0x40};
            bytes[8] = (bytes[8] & std::byte{0x3f}) | std::byte{0x80};
            std::array<std::uint8_t, 16> identity{};
            std::ranges::transform(bytes, identity.begin(), [](std::byte value) {
                return std::to_integer<std::uint8_t>(value);
            });
            return Result<Assets::AssetId>::Success(Assets::AssetId::FromBytes(identity));
        }

        /** @brief Hashes explicit versioned host semantics without test-only provider stamps. */
        Sha256Digest Digest(std::string_view text) {
            return ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
        }

        /** @brief Defines the one native headless output namespace, provider and bounded bake budget. */
        NavigationBakeServiceConfig ServiceConfig(Assets::AssetId definition, const std::filesystem::path &cache,
                                                  const std::filesystem::path &output,
                                                  std::shared_ptr<const Navigation::INavigationMeshBuilder> builder) {
            return {.definition = definition,
                    .artifactType = Assets::AssetTypeId::Parse(Assets::NavMeshAssetTypeName).Value(),
                    .target = AssetCookTargetId::Parse("headless-null").Value(),
                    .cacheRoot = cache,
                    .targetRoot = output,
                    .builder = std::move(builder),
                    .files = std::make_shared<NativeDurableFileSystem>(),
                    .budget = {1, 512ULL * 1024ULL * 1024ULL, 128ULL * 1024ULL * 1024ULL, 8, 1024ULL * 1024ULL * 1024ULL,
                               Duration::FromMilliseconds(30'000)},
                    .sourceAuthority = std::make_shared<NavigationBakeSourceAuthority>(),
                    .newOperationId = NewOperation};
        }

        /** @brief Retains the exact read capability and invocation fences without granting executable package authority. */
        auto CaptureSource(std::shared_ptr<const Platform::IProjectReadFiles> files, ProjectCompatibilitySnapshot compatibility,
                           std::chrono::steady_clock::time_point maximumDeadline, std::shared_ptr<std::atomic<bool>> closed) {
            return [files = std::move(files), compatibility = std::move(compatibility), maximumDeadline,
                    closed = std::move(closed)](const NavigationBakeInvocation &invocation) {
                Platform::ProjectReadContext read{.projectIdentity = compatibility.metadata->projectId,
                                                  .projectGeneration = 1,
                                                  .cancellation = invocation.cancellation,
                                                  .deadline = std::min(maximumDeadline, invocation.deadline),
                                                  .authorityStopped = [closed, stopped = invocation.authorityStopped] {
                    return closed->load() || (stopped && stopped());
                }};
                auto source = NavigationProjectBakeSource::Capture(files, std::move(read), compatibility);
                if (source.HasError())
                    return source;
                if (source.Value()->Settings().Input().defaultProvider != "recast_detour" || source.Value()->Packages().intent ||
                    source.Value()->Packages().lock)
                    return Result<std::shared_ptr<const NavigationProjectBakeSource>>::Failure(
                        MakeError(Nav::NavigationErrors::OperationUnsupported,
                                  "The native navigation command requires the explicit Recast provider and no unadmitted package source"));
                return source;
            };
        }
    }  // namespace

    /** @copydoc NavigationCliHost::~NavigationCliHost */
    NavigationCliHost::~NavigationCliHost() noexcept {
        closed_->store(true);
        if (application_)
            static_cast<void>(application_->Close());
        if (jobs_)
            jobs_->Shutdown(ShutdownPolicy::Cancel);
        if (application_) {
            static_cast<void>(application_->Pump());
            if (application_->Close().HasError())
                std::terminate();
        }
    }

    /** @copydoc NavigationCliHost::Bind */
    Result<std::shared_ptr<NavigationBakeApplication>> NavigationCliHost::Bind(const Cli::CliCommandRequest &request,
                                                                               const Cli::CliExecutionContext &context) {
        using Return = Result<std::shared_ptr<NavigationBakeApplication>>;
        if (context.IsStopRequested())
            return Return::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
        if (application_)
            return Return::Failure(MakeError(Cli::CliErrors::ExecutionContextInvalid));
        auto root = Root(request);
        const auto *option = Option(request, "definition");
        const auto *text = option ? std::get_if<std::string>(option) : nullptr;
        if (root.HasError())
            return Return::Failure(root.ErrorValue());
        if (!text)
            return Return::Failure(MakeError(Cli::CliErrors::ParseFailed));
        auto definition = Assets::AssetId::Parse(*text);
        if (definition.HasError() || !definition.Value().IsValid() || definition.Value().ToString() != *text)
            return Return::Failure(MakeError(Cli::CliErrors::ParseFailed));
        const auto compatibility = InspectProjectCompatibility(root.Value());
        if (!compatibility.metadata || compatibility.markerUpdateRequired ||
            (compatibility.status != ProjectCompatibilityStatus::Current &&
             compatibility.status != ProjectCompatibilityStatus::CompatibleReleaseLine))
            return Return::Failure(compatibility.diagnostic.value_or(MakeError(Nav::NavigationErrors::BakeInputInvalid)));
        auto files = Platform::CreateNativeProjectReadFiles(root.Value(), compatibility.metadata->projectId, 1);
        if (files.HasError())
            return Return::Failure(files.ErrorValue());
        const auto relative = std::filesystem::path{definition.Value().ToString()} / "headless-null";
        auto cache = Output(root.Value(), std::filesystem::path{".horo/cache/navigation"} / relative);
        auto output = Output(root.Value(), std::filesystem::path{".horo/cooked/navigation"} / relative);
        if (cache.HasError() || output.HasError())
            return Return::Failure(cache.HasError() ? cache.ErrorValue() : output.ErrorValue());
        auto builder = Nav::CreateRecastDetourNavigationMeshBuilder();
        if (builder.HasError())
            return Return::Failure(builder.ErrorValue());
        identity_ = ProjectProtocolIdentity(root.Value());
        const auto maximumDeadline = context.AbsoluteDeadline().value_or(std::chrono::steady_clock::time_point::max());
        NavigationBakeApplicationConfig
            config{.service = ServiceConfig(definition.Value(), cache.Value(), output.Value(), std::move(builder).Value()),
                   .compatibility = {.provider = Digest(HORO_NAVIGATION_BUILD_FINGERPRINT),
                                     .schemas = Digest("horo.navigation.automation.v1;scene3;navdef1;hnt2;hns4;canonical-rh-y-up-meters"),
                                     .settings = Digest("horo.navigation.command.v1;headless-null;recast_detour")},
                   .captureSource = CaptureSource(std::move(files).Value(), compatibility, maximumDeadline, closed_)};
        jobs_.emplace(JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 32, .ioWorkerCount = 1});
        auto created = NavigationBakeApplication::Create(std::move(config), operations_, *jobs_);
        if (created.HasError())
            return Return::Failure(created.ErrorValue());
        application_ = std::move(created).Value();
        return Return::Success(application_);
    }

    /** @copydoc NavigationCliHost::BindMcp */
    Result<McpNavigationBinding> NavigationCliHost::BindMcp(const Cli::CliCommandRequest &request,
                                                            const Cli::CliExecutionContext &context) {
        auto application = Bind(request, context);
        if (application.HasError())
            return Result<McpNavigationBinding>::Failure(application.ErrorValue());
        return Result<McpNavigationBinding>::Success({.application = std::move(application).Value(),
                                                      .projectIdentity = identity_,
                                                      .generation = 1,
                                                      .currentGeneration =
                                                          [closed = closed_] {
            return closed->load() ? 0U : 1U;
        },
                                                      .pump = [application = application_] {
            return application->Pump();
        }});
    }
}  // namespace Horo::Application::Internal
