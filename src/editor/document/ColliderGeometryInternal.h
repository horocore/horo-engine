/** @file ColliderGeometryInternal.h
 * @brief Target-private geometry validation shared by analytic collider editing and visualization.
 */
#pragma once

#include "Horo/Editor/PhysicsInspector.h"

namespace Horo::Editor::ColliderEditingDetail {
    /** @brief Validate detached analytic geometry and compose its authored shape-to-world transform.
     * @param collider Canonical analytic shape, local pose/scale and explicit body transform.
     * @return Shape-to-world transform, or the original geometry/diagnostic rejection.
     */
    [[nodiscard]] Result<Math::Mat4> ShapeMatrix(const ColliderPresentation &collider);
}  // namespace Horo::Editor::ColliderEditingDetail
