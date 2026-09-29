#pragma once

/**
 * @file NavigationObstacleOverlay.h
 * @brief Bounded provider-neutral blocker probes and changed-region evidence over immutable Scene snapshots.
 */

#include "Horo/Navigation/NavigationDynamicRegistry.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::Navigation {
    /** @brief One conservative planar region affected by an obstacle publication. */
    struct NavigationObstacleOverlayChangedRegion final {
        NavigationObstacleId obstacle;
        double minimumX{};
        double minimumZ{};
        double maximumX{};
        double maximumZ{};
    };

    /** @brief Result of a bounded segment probe against one immutable logical blocker generation. */
    struct NavigationObstacleOverlayProbe final {
        NavigationDynamicSceneBinding binding;
        NavigationDynamicRegistryRevision revision;
        NavigationObstacleId blockingObstacle;
        bool blocked{};
    };

    /**
     * @brief Conservatively tests a movement/path segment against enabled Scene blockers without touching cooked topology.
     * @param snapshot One retained immutable dynamic publication.
     * @param from Finite world-local segment start.
     * @param to Finite world-local segment end.
     * @param radiusMeters Finite nonnegative agent clearance radius.
     * @param layers Nonempty consumer layer mask.
     * @param maximumChecks Declared bound on records inspected, at most the registry hard obstacle ceiling.
     * @return Blocker identity and exact observed overlay revision, or a typed invalid/capacity error.
     * @details The first matching blocker follows snapshot priority/identity order. A negative answer proves only this
     * retained revision, not that a later publication remains clear. Character/Physics still owns collision authority.
     */
    [[nodiscard]] Result<NavigationObstacleOverlayProbe> ProbeNavigationObstacleOverlay(const NavigationDynamicRegistrySnapshot &snapshot,
                                                                                        Math::Vec3 from, Math::Vec3 to, float radiusMeters,
                                                                                        NavigationDynamicLayerMask layers,
                                                                                        std::size_t maximumChecks);

    /**
     * @brief Projects complete conservative changed regions for path invalidation after one Scene-safe-point publication.
     * @param previous Earlier immutable publication, or invalid for first publication.
     * @param current New immutable publication from the same Scene/world incarnation.
     * @param output Caller-owned bounded region buffer; insufficiency fails without writing any element.
     * @return Number of changed regions written, ordered by stable obstacle identity.
     * @details Each region covers both old and new bounds for a moved obstacle. Callers correlate the returned regions
     * with current.Revision() and invalidate intersecting path dependencies; this function does not mutate topology.
     */
    [[nodiscard]] Result<std::size_t> CollectNavigationObstacleOverlayChanges(const NavigationDynamicRegistrySnapshot &previous,
                                                                              const NavigationDynamicRegistrySnapshot &current,
                                                                              std::span<NavigationObstacleOverlayChangedRegion> output);
}  // namespace Horo::Navigation
