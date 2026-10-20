#pragma once

/** @file NavigationProjectBakeInput.h
 * @brief Canonical complete project-to-bake input capture shared by interactive and automation hosts.
 */

#include "Horo/Application/NavigationSceneBakeGeometry.h"
#include "Horo/Navigation/NavigationTileDependencies.h"

namespace Horo::Application {
    /** @brief Full persistent source key retained alongside compact Navigation identities to detect collisions. */
    struct NavigationProjectBakeSourceKey final {
        Assets::AssetId scene;
        SceneSource::SceneObjectId object;
        Runtime::PhysicsComponentId component;
        Runtime::PhysicsColliderSlotId collider;
        Navigation::SurfaceId surface;
        Runtime::PhysicsBodyReference body;
        [[nodiscard]] auto operator<=>(const NavigationProjectBakeSourceKey &) const noexcept = default;
    };

    /** @brief Exact authored ownership and current source evidence for one immutable geometry contribution. */
    struct NavigationProjectBakeContribution final {
        Navigation::NavigationSourceObservation observation;
        NavigationProjectBakeSourceKey key;
        Assets::AssetId material;
    };

    /** @brief Reducible project bounds; geometry counts and retained bytes apply across all captured Scenes. */
    struct NavigationProjectBakeInputLimits final {
        NavigationSceneGeometryPolicy scene;
        Navigation::NavigationBakeInputLimits input;
        std::size_t maximumSurfaces{4096};
        std::size_t maximumTiles{1024};
    };

    /** @brief Complete owned definition candidate; not a trust decision or permission to publish.
     * @details Source authority, immutable source snapshot and full stable keys remain owned throughout execution.
     * Hosts must revalidate the retained source under their final adoption/mutation fence and use the shared bake
     * service/cache. Geometry capture performs no provider discovery, cache writes or runtime activation.
     */
    struct NavigationProjectBakeInput final {
        std::shared_ptr<const NavigationProjectBakeSource> source;
        Assets::AssetId definition;
        std::shared_ptr<const Navigation::NavigationBakeInputSnapshot> input;
        std::vector<Navigation::NavigationBakeTile> tiles;
        std::vector<NavigationProjectBakeContribution> contributions;
        std::vector<Navigation::NavigationBakeSurfaceKey>
            surfaces; /**< Complete authored catalog, including disabled and empty surfaces. */
    };

    /** @brief Captures all registered Scenes for one exact definition into the canonical bake boundary.
     * @param source Owned root-bound project capture, including material payloads and exact source bytes.
     * @param definition Exact registered NAVDEF01 asset; no first/default definition is inferred.
     * @param revisions Actual host-owned capture revisions, including the authoritative project profile revision.
     * @param limits Qualified aggregate source/input/grid bounds; excess fails without truncation.
     * @param cancellation Invocation cancellation checked during graph, geometry and grid work.
     * @return Complete Scene-qualified input, sorted tile closure and full source ownership, or typed failure.
     * @details Uses exact Physics collider geometry and material codecs. Compact producer/contribution IDs are
     * versioned canonical hashes of full persistent keys; collisions and reserved zero fail rather than aliasing.
     * LocalBounds surfaces retain their complete transformed grid, including empty clipped geometry. Subtree
     * surfaces use their complete source footprint. Each profile uses its own authored voxel/tile extent and
     * finite agent clearance in Y. Unsupported authored modifier/link assembly currently fails explicitly;
     * those semantics must be composed before this candidate can serve the complete production command contract.
     * Registry/source bytes are checked before and after capture; this is not the later publication lease.
     */
    [[nodiscard]] Result<NavigationProjectBakeInput> CaptureNavigationProjectBakeInput(
        std::shared_ptr<const NavigationProjectBakeSource> source, Assets::AssetId definition,
        const Navigation::NavigationBakeInputRevisions &revisions, const NavigationProjectBakeInputLimits &limits = {},
        CancellationToken cancellation = {});
}  // namespace Horo::Application
