#include "Horo/Debug/ConsoleStartup.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>

namespace Horo::Debug {
    /** @copydoc ConsoleLaunchArguments::Parse */
    Result<ConsoleLaunchArguments> ConsoleLaunchArguments::Parse(const std::span<const std::string_view> arguments) {
        if (arguments.size() > 256)
            return Result<ConsoleLaunchArguments>::Failure(MakeCommandError(CommandError::LimitExceeded));
        ConsoleLaunchArguments captured;
        std::array<bool, static_cast<std::size_t>(OverlaySource::Count)> panels{};
        bool allDefaults{};
        bool specificPanels{};
        std::size_t totalBytes{};
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto argument = arguments[i];
            if (argument.size() > 4096 || totalBytes > 65536 - argument.size())
                return Result<ConsoleLaunchArguments>::Failure(MakeCommandError(CommandError::LimitExceeded));
            if (!IsValidUtf8ScalarSequence(argument) || argument.find('\0') != std::string_view::npos)
                return Result<ConsoleLaunchArguments>::Failure(MakeCommandError(CommandError::Usage));
            totalBytes += argument.size();
            if (argument == "--console")
                captured.inputs_.insert_or_assign(SettingKey(std::string(ConsoleEnabledSettingName)), ConfigurationInputValue{true, {}});
            else if (argument == "--dev-overlays") {
                allDefaults = true;
                captured.inputs_.insert_or_assign(SettingKey(std::string(OverlaysEnabledSettingName)), ConfigurationInputValue{true, {}});
            } else if (argument.starts_with("--dev-overlay=")) {
                const auto panel = ParseOverlaySource(argument.substr(14));
                if (!panel || panels[static_cast<std::size_t>(*panel)])
                    return Result<ConsoleLaunchArguments>::Failure(MakeCommandError(CommandError::Usage));
                panels[static_cast<std::size_t>(*panel)] = true;
                specificPanels = true;
                captured.inputs_.insert_or_assign(SettingKey(std::string(OverlaysEnabledSettingName)), ConfigurationInputValue{true, {}});
            } else if (argument == "--exec" || argument == "--console-script") {
                if (++i == arguments.size() || arguments[i].empty() || arguments[i].size() > 4096 ||
                    totalBytes > 65536 - arguments[i].size())
                    return Result<ConsoleLaunchArguments>::Failure(MakeCommandError(CommandError::Usage));
                if (!IsValidUtf8ScalarSequence(arguments[i]) || arguments[i].find('\0') != std::string_view::npos)
                    return Result<ConsoleLaunchArguments>::Failure(MakeCommandError(CommandError::Usage));
                totalBytes += arguments[i].size();
                if (argument == "--exec")
                    captured.startup_.push_back({ConsoleStartupKind::Command, std::string(arguments[i])});
                else {
                    if (!Platform::IsSafeProjectReadPath(arguments[i]))
                        return Result<ConsoleLaunchArguments>::Failure(MakeCommandError(CommandError::StartupDenied));
                    captured.startup_.push_back({ConsoleStartupKind::Script, std::string(arguments[i])});
                }
            } else if (argument == "--remote-console" || argument.starts_with("--remote-console=")) {
                return Result<ConsoleLaunchArguments>::Failure(MakeCommandError(CommandError::MissingCapability));
            } else
                return Result<ConsoleLaunchArguments>::Failure(MakeCommandError(CommandError::Usage));
        }
        if (specificPanels)
            for (std::size_t i = 0; i < panels.size(); ++i)
                if (panels[i] || !allDefaults)
                    captured.inputs_.insert_or_assign(*ConsoleOverlayVisibilityKey(static_cast<OverlaySource>(i)),
                                                      ConfigurationInputValue{panels[i], {}});
        return Result<ConsoleLaunchArguments>::Success(std::move(captured));
    }

    /** @copydoc ConsoleLaunchArguments::ConfigurationInputs */
    const ConfigurationSourceMap &ConsoleLaunchArguments::ConfigurationInputs() const noexcept {
        return inputs_;
    }

    /** @copydoc ConsoleLaunchArguments::Startup */
    std::span<const ConsoleStartupRequest> ConsoleLaunchArguments::Startup() const noexcept {
        return startup_;
    }

    /** @copydoc ResolveConsoleLaunch */
    Result<ConsoleLaunchConfiguration> ResolveConsoleLaunch(const ConsoleLaunchArguments &arguments,
                                                            const ConfigurationSnapshotRef &snapshot, const ProductProfile profile,
                                                            const bool shippingUi) {
        if (static_cast<unsigned>(profile) > static_cast<unsigned>(ProductProfile::Server))
            return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        auto defaults = ReadConsoleConfiguration(snapshot);
        if (defaults.HasError())
            return Result<ConsoleLaunchConfiguration>::Failure(defaults.ErrorValue());
        for (const auto &[key, input] : arguments.ConfigurationInputs()) {
            const auto *entry = snapshot.FindResolved(key);
            if (!entry || entry->source != ConfigurationSource::Invocation || entry->value != input.value)
                return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        }
        if (!defaults.Value().startupAllowed && !arguments.Startup().empty())
            return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::StartupDenied));
        ConsoleLaunchConfiguration configuration;
        configuration.console = defaults.Value().console;
        configuration.overlays = defaults.Value().overlays;
        configuration.historyLimit = defaults.Value().historyLimit;
        configuration.configurationRevision = defaults.Value().revision;
        if (configuration.overlays)
            configuration.panels = defaults.Value().panels;
        if (profile != ProductProfile::Editor && profile != ProductProfile::Development &&
            std::ranges::find(configuration.panels, OverlaySource::Scene) != configuration.panels.end())
            return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::PermissionDenied));
        if ((profile == ProductProfile::Shipping && (configuration.overlays || (configuration.console && !shippingUi))) ||
            (profile == ProductProfile::Server && (configuration.console || configuration.overlays)))
            return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::PermissionDenied));
        configuration.startup.assign(arguments.Startup().begin(), arguments.Startup().end());
        return Result<ConsoleLaunchConfiguration>::Success(std::move(configuration));
    }

    /** @copydoc ReadConsoleScript */
    Result<std::vector<std::string>> ReadConsoleScript(const std::string_view path, const std::span<const std::string_view> approvedPaths,
                                                       const Platform::IProjectReadFiles &files,
                                                       const Platform::ProjectReadContext &readContext, const CommandRegistry &registry,
                                                       const DebugCommandContext &principal) {
        if (approvedPaths.size() > 256 || !Platform::IsSafeProjectReadPath(path) ||
            std::ranges::find(approvedPaths, path) == approvedPaths.end())
            return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::StartupDenied));
        Platform::ProjectReadLimits limits;
        limits.maximumFileBytes = 65536;
        auto captured = files.Text(path, readContext, limits);
        if (captured.HasError())
            return Result<std::vector<std::string>>::Failure(captured.ErrorValue());
        const auto &snapshot = captured.Value();
        if (!snapshot.text || snapshot.text->size() > limits.maximumFileBytes || snapshot.path != path ||
            snapshot.projectIdentity != readContext.projectIdentity || snapshot.projectGeneration != readContext.projectGeneration ||
            !IsValidUtf8ScalarSequence(*snapshot.text))
            return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::StartupDenied));
        std::vector<std::string> commands;
        std::string_view remaining(*snapshot.text);
        while (!remaining.empty()) {
            const auto newline = remaining.find('\n');
            auto line = remaining.substr(0, newline);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.find_first_not_of(" \t") != std::string_view::npos) {
                if (commands.size() == 256)
                    return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::LimitExceeded));
                const auto prepared = registry.Prepare(line, principal, true);
                if (prepared.HasError())
                    return Result<std::vector<std::string>>::Failure(prepared.ErrorValue());
                commands.emplace_back(line);
            }
            if (newline == std::string_view::npos)
                break;
            remaining.remove_prefix(newline + 1);
        }
        const auto live = Platform::CheckProjectReadContext(readContext);
        if (live.HasError())
            return Result<std::vector<std::string>>::Failure(live.ErrorValue());
        return Result<std::vector<std::string>>::Success(std::move(commands));
    }
}  // namespace Horo::Debug
