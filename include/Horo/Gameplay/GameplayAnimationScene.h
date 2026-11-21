#pragma once

/** @file GameplayAnimationScene.h
 * @brief Host-only factory for trusted gameplay animation admission.
 */

#include "Horo/Gameplay/GameplayAnimationContext.h"
#include "Horo/Runtime/Scene/SceneAnimationRuntime.h"

namespace Horo::Gameplay {
    /** @brief Issues a gameplay grant after host policy has allowed animation for the module.
     * @param runtime Weak exact Scene animation owner. @param scene Intended active or pending Scene incarnation.
     * @param moduleId Trusted module principal. @param cancellation Host-owned module generation lifetime.
     * @return Owned module grant or typed admission failure. Invoke on the Scene owner lane.
     */
    [[nodiscard]] Result<std::shared_ptr<const GameplayAnimationContext>> CreateGameplayAnimationContext(
        std::weak_ptr<Runtime::SceneAnimationRuntime> runtime, Runtime::SceneRuntimeId scene, std::string moduleId,
        const CancellationToken &cancellation);
}  // namespace Horo::Gameplay
