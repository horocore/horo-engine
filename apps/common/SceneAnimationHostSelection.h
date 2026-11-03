#pragma once

/** @file SceneAnimationHostSelection.h
 * @brief Explicit application selection shared by Editor and headless Scene compositions.
 */
#include "AnimationRuntimeIdentitySource.h"
#include "Horo/Runtime/Scene/SceneAnimationRuntime.h"

namespace Horo::Application::Internal {
    /** @brief Inert factory inputs; each actual Scene host issues its identity and owns its cancellation lifetime. */
    struct SceneAnimationHostSelection final {
        Runtime::RuntimeDispatchSource dispatch;
        AssetCookTargetId target;
        std::shared_ptr<AnimationRuntimeIdentitySource> identities;
        Runtime::SceneAnimationLimits limits;
        std::shared_ptr<const Runtime::SceneAnimationInputSource> inputs;
    };
}  // namespace Horo::Application::Internal
