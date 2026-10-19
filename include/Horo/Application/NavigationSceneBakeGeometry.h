#pragma once

/** @file NavigationSceneBakeGeometry.h
 * @brief Canonical authored Scene collider extraction for shared navigation automation.
 */

#include "Horo/Application/NavigationPrimitiveGeometry.h"
#include "Horo/Application/NavigationProjectBakeSource.h"

namespace Horo::Application {
    /** @brief Reducible graph and whole-Scene geometry bounds; graph scratch is separately bounded by object count. */
    struct NavigationSceneGeometryPolicy final {
        static constexpr std::size_t MaximumRegions = 4096;
        static constexpr std::uint32_t MaximumRegionFragments = 256;
        static constexpr std::uint64_t MaximumRegionWorkUnits = 64ULL * 1024ULL * 1024ULL;
        std::size_t maximumObjects{65'536};
        std::size_t maximumHierarchyDepth{256};
        NavigationPrimitiveGeometryPolicy geometry;
        std::size_t maximumRegions{MaximumRegions};
        std::uint32_t maximumRegionFragments{MaximumRegionFragments};
        std::uint64_t maximumRegionWorkUnits{MaximumRegionWorkUnits}; /**< Includes count and write passes; shared across project Scenes. */
    };

    /** @brief Owned collider geometry with the complete stable authored key, never a runtime slot or file path. */
    struct NavigationCapturedCollider final {
        Assets::AssetId scene;
        SceneSource::SceneObjectId object;
        Runtime::PhysicsComponentId component;
        Runtime::PhysicsColliderSlotId collider;
        Runtime::PhysicsBodyReference body;
        Runtime::NavigationSourceComponent source;
        NavigationPrimitiveGeometry geometry;
        std::optional<Math::Aabb>
            footprint;              /**< Complete source scope before region selection; empty output does not erase grid coverage. */
        Sha256Digest contentDigest; /**< Geometry plus exact applicable authored region semantics; distinct from geometry-only digest. */
    };

    /** @brief Extracts every enabled explicit collider source for one exact definition in stable authored-key order.
     * @param scene Canonically decoded registered Scene and its stable asset identity.
     * @param definition Exact registered definition; profiles, areas and filters are resolved without defaults.
     * @param policy Finite graph, faceting and aggregate retained-geometry limits.
     * @param cancellation Invocation cancellation checked during graph and geometry work.
     * @return Complete owned collider contributions, including empty clipped intersections, or typed failure.
     * @details Validates the complete object graph and navigation/Physics references before extraction. Sources
     * require enabled non-sensor colliders on enabled static bodies with unit, scale-free world poses. Collider
     * Definitions must declare canonical Horo right-handed Y-up metre coordinates; foreign conventions fail.
     * Collider local poses are relative to the referenced body, irrespective of the containing object's transform.
     * ObjectSubtree scopes require authored ancestry; LocalBounds scopes clip in the surface owner's exact
     * affine local frame. Infinite planes require LocalBounds. Analytic sources require one explicit material
     * slot representable by Navigation; no slot or asset is inferred. Material asset trust/availability,
     * package admission, modifier/link assembly and final source/publication fences remain host work.
     * Enabled include regions form a union over the explicitly eligible source pool; exclusions then subtract
     * their exact affine local boxes. Both current selection modes admit these explicit static-collider sources.
     * Overlaps do not duplicate geometry. Fragment, count, peak geometry-byte and work budgets fail closed.
     * Unknown components, unexpanded prefabs and unqualified shape artifacts fail rather than being omitted.
     * This synchronous tooling operation creates no world and performs no I/O, migration or cache writes.
     */
    [[nodiscard]] Result<std::vector<NavigationCapturedCollider>> CaptureNavigationSceneColliderGeometry(
        const NavigationSceneBakeSource &scene, const NavigationDefinitionBakeSource &definition,
        const NavigationSceneGeometryPolicy &policy = {}, CancellationToken cancellation = {});
}  // namespace Horo::Application
