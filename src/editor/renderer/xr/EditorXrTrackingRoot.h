#pragma once
/** @file EditorXrTrackingRoot.h
 * @brief Non-installed exact authored tracking-root extraction over canonical Scene world transforms.
 */
#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::Editor {
    /** @brief Resolves one explicitly selected authored object's native-base-to-World rigid placement.
     * @param scene Exact current or unpublished Scene candidate; no borrow escapes this call.
     * @param object Explicit object in that Scene, never a camera/root fallback.
     * @return Finite unit-scale positive-orientation transform, or the original missing/stale/extraction error.
     * @details Hierarchy composition uses the existing canonical scene extractor. Scale, shear, reflection and
     * projective transforms are rejected, with a 1e-5 floating-point orthonormality tolerance. The retained value
     * maps native base coordinates to World; OpenXR reference-space creation consumes its inverse.
     */
    [[nodiscard]] Result<Math::Transform> ResolveXrTrackingRoot(Runtime::RuntimeSceneView scene, Runtime::SceneObjectId object);
}  // namespace Horo::Editor
