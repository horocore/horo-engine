#pragma once

/** @file EditorConsoleLaunch.h @brief Non-installed graphical-host argument ownership and canonical console input composition. */
#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Debug/ConsoleStartupExecution.h"

namespace Horo::Application::Internal {
    /** @brief Inert explicit host capabilities whose real owners must be supplied before dispatcher activation. */
    struct EditorConsoleCapabilities final {
        bool networkInspection{};
    };

    /** @brief Console capture plus the unchanged relative order of other host arguments.
     * Host pointers borrow the process argv storage through synchronous editor bootstrap; console capture owns its input.
     */
    struct EditorConsoleLaunch final {
        std::vector<char *> hostArguments;
        Debug::ConsoleLaunchArguments console;
    };

    /** @brief Owned inert bound inventory and fully checked startup sequence, prepared before native host publication.
     * This bootstrap admits only actually bound backend-independent commands; it is not the complete default command catalog.
     */
    struct EditorConsolePreparation final {
        std::shared_ptr<const Debug::CommandRegistry> registry;
        std::shared_ptr<Debug::ConsoleHistory> history;
        Debug::PreparedConsoleStartup startup;
        bool consoleVisible{}; /**< One initial request seeded into the active session variable at explicit activation. */
    };

    /** @brief Prepares the Editor core/Telemetry-availability/Scene-read inventory and local startup policy without invoking handlers.
     * @param launch Resolved canonical request. @param configuration Same immutable host revision.
     * @param capabilities Explicit optional host inventory; construction never discovers or queries its owners.
     * @return Owned preparation or original typed policy/input failure, before any native presentation is created.
     * Only the four core commands are startup-allowlisted. Filesystem scripts require a separately composed exact-path project authority.
     */
    [[nodiscard]] Result<EditorConsolePreparation> PrepareEditorConsole(const Debug::ConsoleLaunchConfiguration &launch,
                                                                        const ConfigurationSnapshotRef &configuration,
                                                                        EditorConsoleCapabilities capabilities = {});

    /** @brief Separates console-owned tokens before renderer/XR/editor parsers inspect the host invocation.
     * @param arguments Process argv including the executable. Known host option values remain owned by their original parser.
     * @return Finite owned console capture and borrowed host arguments, or typed malformed/oversized/remote request failure.
     * Console command/script payloads are opaque to all other option parsers, even when they resemble host switches.
     */
    [[nodiscard]] Result<EditorConsoleLaunch> CaptureEditorConsoleLaunch(std::span<char *> arguments);

    /** @brief Adds only captured Invocation candidates to the host's existing canonical request, rejecting ownership collisions.
     * @param console Immutable captured console inputs. @param sources Host-owned complete resolution inputs.
     * @return Success or duplicate-key failure without mutating sources when a collision is detected.
     */
    [[nodiscard]] Result<void> MergeEditorConsoleInputs(const Debug::ConsoleLaunchArguments &console,
                                                        ConfigurationResolutionRequest &sources);
}  // namespace Horo::Application::Internal
