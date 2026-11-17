#include "EditorConsoleLaunch.h"

#include "NetworkConsoleInspection.h"

#include <algorithm>
#include <array>

namespace Horo::Application::Internal {
    namespace {
        // Root-owned token arity only. Each existing subsystem still validates and interprets its own values.
        constexpr std::array<std::string_view, 22> HostValueOptions{"--renderer",          "--project",
                                                                    "--physics",           "--prefab",
                                                                    "--cooked-root",       "--cook-target",
                                                                    "--frame-rate-limit",  "--exit-after-frames",
                                                                    "--xr-tracking-root",  "--lighting",
                                                                    "--lighting-fallback", "--lighting-coverage",
                                                                    "--lighting-root",     "--color-pipeline-root",
                                                                    "--color-look-root",   "--exposure-mode",
                                                                    "--exposure-ev",       "--exposure-compensation",
                                                                    "--shadow-quality",    "--xr-host-roots",
                                                                    "--xr-host-catalog",   "--xr-host-catalog-signature"};

        /** @brief Assigns the console's reserved option spellings, including malformed variants, to its sole parser. */
        bool IsConsoleOption(std::string_view value) {
            return value == "--console" || value.starts_with("--console=") || value.starts_with("--console-") ||
                   value == "--dev-overlays" || value.starts_with("--dev-overlays=") || value == "--dev-overlay" ||
                   value.starts_with("--dev-overlay=") || value == "--exec" || value.starts_with("--exec=") ||
                   value == "--remote-console" || value.starts_with("--remote-console=");
        }
    }  // namespace

    /** @copydoc PrepareEditorConsole */
    Result<EditorConsolePreparation> PrepareEditorConsole(const Debug::ConsoleLaunchConfiguration &launch,
                                                          const ConfigurationSnapshotRef &configuration,
                                                          const EditorConsoleCapabilities capabilities) {
        const auto settings = Debug::ReadConsoleConfiguration(configuration);
        if (settings.HasError())
            return Result<EditorConsolePreparation>::Failure(settings.ErrorValue());
        if (launch.historyLimit != settings.Value().historyLimit)
            return Result<EditorConsolePreparation>::Failure(Debug::MakeCommandError(Debug::CommandError::InvalidDescriptor));
        Debug::RegistryPolicy policy;
        policy.profile = Debug::ProductProfile::Editor;
        policy.limits.historyEntries = settings.Value().historyLimit;
        policy.startupAllowlist = {Debug::HelpCommandId, Debug::FindCommandId, Debug::VersionCommandId, Debug::ClearCommandId};
        const auto core = Debug::CoreConsoleDescriptors();
        std::vector<Debug::DebugCommandDescriptor> commands(core.begin(), core.end());
        commands.push_back(Debug::ConsoleMetricDescriptor());
        const auto sceneCommands = Debug::ConsoleSceneDescriptors();
        commands.insert(commands.end(), sceneCommands.begin(), sceneCommands.end());
        if (capabilities.networkInspection)
            commands.push_back(NetworkConsoleStatusDescriptor());
        auto created = Debug::CommandRegistry::Create(commands, policy);
        if (created.HasError())
            return Result<EditorConsolePreparation>::Failure(created.ErrorValue());
        auto registry = std::make_shared<Debug::CommandRegistry>(std::move(created).Value());
        auto startup = Debug::PreparedConsoleStartup::Prepare(launch, configuration, registry, {});
        if (startup.HasError())
            return Result<EditorConsolePreparation>::Failure(startup.ErrorValue());
        return Result<EditorConsolePreparation>::Success(
            {std::move(registry), std::make_shared<Debug::ConsoleHistory>(policy.limits), std::move(startup).Value(), launch.console});
    }

    /** @copydoc CaptureEditorConsoleLaunch */
    Result<EditorConsoleLaunch> CaptureEditorConsoleLaunch(const std::span<char *> arguments) {
        if (arguments.empty())
            return Result<EditorConsoleLaunch>::Failure(Debug::MakeCommandError(Debug::CommandError::Usage));
        if (arguments.size() > 4096)
            return Result<EditorConsoleLaunch>::Failure(Debug::MakeCommandError(Debug::CommandError::LimitExceeded));
        std::size_t remaining = 1U << 20U;
        for (const char *argument : arguments) {
            if (!argument)
                return Result<EditorConsoleLaunch>::Failure(Debug::MakeCommandError(Debug::CommandError::Usage));
            const auto bytes = std::string_view(argument).size();
            if (bytes > remaining)
                return Result<EditorConsoleLaunch>::Failure(Debug::MakeCommandError(Debug::CommandError::LimitExceeded));
            remaining -= bytes;
        }
        std::vector<char *> host;
        host.reserve(arguments.size());
        host.push_back(arguments.front());
        std::array<std::string_view, 256> console;
        std::size_t count{};
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view option(arguments[index]);
            if (std::ranges::find(HostValueOptions, option) != HostValueOptions.end()) {
                host.push_back(arguments[index]);
                if (index + 1 < arguments.size())
                    host.push_back(arguments[++index]);
                continue;
            }
            if (!IsConsoleOption(option)) {
                host.push_back(arguments[index]);
                continue;
            }
            if (count == console.size())
                return Result<EditorConsoleLaunch>::Failure(Debug::MakeCommandError(Debug::CommandError::LimitExceeded));
            console[count++] = option;
            if (option != "--exec" && option != "--console-script")
                continue;
            if (index + 1 == arguments.size())
                return Result<EditorConsoleLaunch>::Failure(Debug::MakeCommandError(Debug::CommandError::Usage));
            if (count == console.size())
                return Result<EditorConsoleLaunch>::Failure(Debug::MakeCommandError(Debug::CommandError::LimitExceeded));
            console[count++] = arguments[++index];
        }
        auto captured = Debug::ConsoleLaunchArguments::Parse(std::span(console).first(count));
        if (captured.HasError())
            return Result<EditorConsoleLaunch>::Failure(captured.ErrorValue());
        return Result<EditorConsoleLaunch>::Success({std::move(host), std::move(captured).Value()});
    }

    /** @copydoc MergeEditorConsoleInputs */
    Result<void> MergeEditorConsoleInputs(const Debug::ConsoleLaunchArguments &console, ConfigurationResolutionRequest &sources) {
        for (const auto &[key, value] : console.ConfigurationInputs()) {
            (void)value;
            if (sources.invocation.contains(key))
                return Result<void>::Failure(Debug::MakeCommandError(Debug::CommandError::Duplicate));
        }
        sources.invocation.insert(console.ConfigurationInputs().begin(), console.ConfigurationInputs().end());
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
