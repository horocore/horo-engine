#pragma once

/** @file NavigationPrimitiveGeometry.h
 * @brief Bounded offline conversion of canonical analytic collision shapes to navigation triangles.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Navigation/NavigationSourceGeometry.h"
#include "Horo/Physics/PhysicsShapeDescriptor.h"

#include <optional>

namespace Horo::Application {
    /** @brief Explicit faceting and storage limits; used only during tooling/background capture. */
    struct NavigationPrimitiveGeometryPolicy final {
        float maximumChordErrorMeters{0.01F};
        std::uint32_t maximumSegments{1024};
        Navigation::NavigationSourceGeometryLimits storage;
        std::optional<Math::Aabb> planeBounds; /**< Required canonical world bounds for infinite planes; never inferred. */
    };

    /** @brief Owned canonical metre geometry; no runtime world, native handles, or borrowed source storage. */
    struct NavigationPrimitiveGeometry final {
        std::vector<Math::Vec3> vertices;
        std::vector<Navigation::NavigationSourceTriangleInput> triangles;
        Sha256Digest digest;
    };

    /** @brief Resolves authored primitive scale and facets bounded finite shapes in canonical world metres.
     * @param primitive Physics-owned analytic geometry, collider-local pose, and authored scale.
     * @param bodyWorldPose Valid scale-free world pose of the explicitly resolved static body owner.
     * @param area Exact authored navigation area; no default area is inferred.
     * @param material Exact source material slot already admitted by the caller.
     * @param policy Positive chord-error tolerance and reducible hard count/byte bounds.
     * @param cancellation Invocation cancellation, checked before allocation and throughout generation.
     * @return Owned outward-wound geometry and versioned canonical content hash, or typed failure.
     * @details This function does not prove body motion, source eligibility, material asset validity, trust,
     * or source freshness. Those remain capture-owner admission. Infinite planes require explicit finite world bounds; a disjoint
     * intersection returns empty owned geometry. Rounded surfaces use a conservative angular sagitta bound. Numerically
     * collapsed/overflowing output is rejected.
     */
    [[nodiscard]] Result<NavigationPrimitiveGeometry> CaptureNavigationPrimitiveGeometry(
        const Physics::PhysicsPrimitiveShapeRequest &primitive, const Physics::PhysicsPose &bodyWorldPose,
        Navigation::NavigationAreaId area, Navigation::NavigationSourceMaterialSlot material,
        const NavigationPrimitiveGeometryPolicy &policy = {}, CancellationToken cancellation = {});
}  // namespace Horo::Application
