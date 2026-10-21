#include "NavigationCliHost.h"

#include "Horo/Cli/CliErrors.h"
#include "NativeNavigationBakeComposition.h"
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

        /** @brief Parses the required canonical asset identifier before any native capability is created. */
        Result<Assets::AssetId> Definition(const Cli::CliCommandRequest &request) {
            const auto *option = Option(request, "definition");
            const auto *text = option ? std::get_if<std::string>(option) : nullptr;
            if (!text)
                return Result<Assets::AssetId>::Failure(MakeError(Cli::CliErrors::ParseFailed));
            auto definition = Assets::AssetId::Parse(*text);
            if (definition.HasError() || !definition.Value().IsValid() || definition.Value().ToString() != *text)
                return Result<Assets::AssetId>::Failure(MakeError(Cli::CliErrors::ParseFailed));
            return definition;
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
        if (root.HasError())
            return Return::Failure(root.ErrorValue());
        auto definition = Definition(request);
        if (definition.HasError())
            return Return::Failure(definition.ErrorValue());
        const auto compatibility = InspectProjectCompatibility(root.Value());
        if (!compatibility.metadata || compatibility.markerUpdateRequired ||
            (compatibility.status != ProjectCompatibilityStatus::Current &&
             compatibility.status != ProjectCompatibilityStatus::CompatibleReleaseLine))
            return Return::Failure(compatibility.diagnostic.value_or(MakeError(Nav::NavigationErrors::BakeInputInvalid)));
        identity_ = ProjectProtocolIdentity(root.Value());
        auto config = ComposeNativeNavigationBake(root.Value(), definition.Value(), compatibility, closed_,
                                                  context.AbsoluteDeadline().value_or(std::chrono::steady_clock::time_point::max()));
        if (config.HasError())
            return Return::Failure(config.ErrorValue());
        jobs_.emplace(JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 32, .ioWorkerCount = 1});
        auto created = NavigationBakeApplication::Create(std::move(config).Value(), operations_, *jobs_);
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
