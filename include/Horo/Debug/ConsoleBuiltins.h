#pragma once

/** @file ConsoleBuiltins.h @brief Explicit host composition for safe, backend-independent console commands. */
#include "Horo/Debug/CommandExecutor.h"

namespace Horo::Debug {
    class ConsoleVariableSet;
    /** @brief Stable engine-owned core command identities; projects must not reuse these IDs. */
    inline constexpr DebugCommandId HelpCommandId = 0xDB600001;
    inline constexpr DebugCommandId FindCommandId = 0xDB600002;
    inline constexpr DebugCommandId VersionCommandId = 0xDB600003;
    inline constexpr DebugCommandId ClearCommandId = 0xDB600004;
    /** @brief Returns inert Public/ShippingAllowlist metadata for help, find, version and clear.
     * @return Process-lifetime metadata only; neither registers handlers nor starts a service.
     */
    [[nodiscard]] std::span<const DebugCommandDescriptor> CoreConsoleDescriptors() noexcept;
    /** @brief Binds backend-independent commands to the exact registry and visible history authority.
     * @param registry Composed immutable inventory. @param history Existing visible command/result history.
     * @param version Privacy-reviewed build/game/platform/profile identity without source paths.
     * @param variables Optional existing variable inventory; help/find use the same principal gates.
     * @return Owned bindings for present core commands, with no handler side effects during construction.
     * Clear is owner-thread scheduled because visible history belongs to its presentation owner.
     */
    [[nodiscard]] Result<std::vector<CommandBinding>> BindCoreConsoleCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                              std::shared_ptr<ConsoleHistory> history, std::string version,
                                                                              std::shared_ptr<const ConsoleVariableSet> variables = {});
}  // namespace Horo::Debug
