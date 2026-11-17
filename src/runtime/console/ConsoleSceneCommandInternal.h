#pragma once

/** @file ConsoleSceneCommandInternal.h @brief Non-installed read-only inspection binding for explicitly published Scene views. */
#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <variant>

namespace Horo::Debug::Internal {
    /** @brief Owner-thread publication slot; views weakly observe the existing Scene and never extend storage lifetime.
     * Publish immediately before the command owner cutoff. Retired/stale views fail closed even if retained after shutdown.
     * The owning-service facade instead selects the retained-service alternative, preserving its execution-time acquisition.
     * Neither mode provides a raw service pointer, ownership transfer, alternate scene model or cross-thread access.
     */
    struct ConsoleSceneInspection final {
        std::variant<std::optional<Runtime::RuntimeSceneView>, std::shared_ptr<Runtime::RuntimeSceneService>> source;
    };

    /** @brief Binds admitted read-only commands to the same Scene inspection implementation used by owning-service hosts.
     * @param registry Immutable Editor/Development inventory. @param scene Explicit host-published borrowed view capability.
     * @param limits Bounded inspection work. @return Owned handlers or typed invalid/missing capability failure.
     * @pre Bind, publish and dispatch on one Scene owner thread. Binding queries no Scene state.
     */
    [[nodiscard]] Result<std::vector<CommandBinding>> BindSceneInspectionCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                                  std::shared_ptr<const ConsoleSceneInspection> scene,
                                                                                  ConsoleSceneInspectionLimits limits = {});
}  // namespace Horo::Debug::Internal
