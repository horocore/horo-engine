#pragma once

#include "Horo/Application/NavigationPrimitiveGeometry.h"

namespace Horo::Application::NavigationBakeDetail {
    /** @brief Checks reducible hard geometry bounds without producing or allocating geometry. */
    [[nodiscard]] bool IsValidGeometryPolicy(const NavigationPrimitiveGeometryPolicy &policy) noexcept;
    /** @brief Checks final float triangle positions with double intermediates. */
    [[nodiscard]] bool TriangleHasArea(Math::Vec3 a, Math::Vec3 b, Math::Vec3 c) noexcept;
    /** @brief Validates, transforms and hashes owned geometry using the one canonical analytic content format.
     * @param output Owned candidate. @param local Collider-local placement. @param world Body world placement.
     * @param area Exact area. @param material Exact slot. @param cancellation Invocation cancellation.
     * @return Complete canonical geometry or typed failure, without partial publication.
     */
    [[nodiscard]] Result<NavigationPrimitiveGeometry> FinalizeGeometry(NavigationPrimitiveGeometry output,
                                                                       const Physics::PhysicsPose &local, const Physics::PhysicsPose &world,
                                                                       Navigation::NavigationAreaId area,
                                                                       Navigation::NavigationSourceMaterialSlot material,
                                                                       CancellationToken cancellation);
}  // namespace Horo::Application::NavigationBakeDetail
