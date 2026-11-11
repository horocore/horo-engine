#include "Horo/Debug/ConsoleStartup.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>

namespace Horo::Debug {
    namespace {
        /** @brief Finite canonical panel IDs; absent services are a later host composition error. */
        constexpr std::string_view Panels[]{"logs", "frame", "cpu", "memory", "jobs", "renderer", "assets", "scene", "input", "network"};
    }  // namespace

    /** @copydoc ParseConsoleLaunch */
    Result<ConsoleLaunchConfiguration> ParseConsoleLaunch(const std::span<const std::string_view> arguments, const ProductProfile profile,
                                                          const bool shippingUi) {
        if (arguments.size() > 256 || static_cast<unsigned>(profile) > 5)
            return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::LimitExceeded));
        ConsoleLaunchConfiguration configuration;
        configuration.console = profile == ProductProfile::Development;
        std::size_t totalBytes{};
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto argument = arguments[i];
            if (argument.size() > 4096 || totalBytes > 65536 - argument.size())
                return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::LimitExceeded));
            totalBytes += argument.size();
            if (argument == "--console")
                configuration.console = true;
            else if (argument == "--dev-overlays")
                configuration.overlays = true;
            else if (argument.starts_with("--dev-overlay=")) {
                const auto panel = argument.substr(14);
                if (std::ranges::find(Panels, panel) == std::end(Panels) ||
                    std::ranges::find(configuration.panels, panel) != configuration.panels.end())
                    return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::Usage));
                configuration.overlays = true;
                configuration.panels.emplace_back(panel);
            } else if (argument == "--exec" || argument == "--console-script") {
                if (++i == arguments.size() || arguments[i].empty() || arguments[i].size() > 4096 ||
                    totalBytes > 65536 - arguments[i].size())
                    return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::Usage));
                totalBytes += arguments[i].size();
                if (argument == "--exec")
                    configuration.startupCommands.emplace_back(arguments[i]);
                else {
                    if (!Platform::IsSafeProjectReadPath(arguments[i]))
                        return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::StartupDenied));
                    configuration.scripts.emplace_back(arguments[i]);
                }
            } else if (argument == "--remote-console" || argument.starts_with("--remote-console=")) {
                return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::MissingCapability));
            } else
                return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::Usage));
        }
        if ((profile == ProductProfile::Shipping && (configuration.overlays || (configuration.console && !shippingUi))) ||
            (profile == ProductProfile::Server && (configuration.console || configuration.overlays)))
            return Result<ConsoleLaunchConfiguration>::Failure(MakeCommandError(CommandError::PermissionDenied));
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
