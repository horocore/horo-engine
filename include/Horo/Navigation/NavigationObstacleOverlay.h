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
        SurfaceId surface; /**< Invalid for a Scene-wide region; set for a surface projection. */
        double minimumX{};
        double minimumZ{};
        double maximumX{};
        double maximumZ{};
    };

    /** @brief Result of a bounded segment probe against one immutable logical blocker generation. */
    struct NavigationObstacleOverlayProbe final {
        NavigationDynamicSceneBinding binding;
        NavigationDynamicRegistryRevision revision;
        SurfaceId surface;             /**< Invalid for a Scene-wide probe; set for a surface-local probe. */
        NavigationGeneration topology; /**< Invalid for a Scene-wide probe; exact caller-supplied surface generation otherwise. */
        NavigationObstacleId blockingObstacle;
        bool blocked{};
    };

    /** @brief One topology-owner-supplied surface extent and exact world/topology fence in canonical Scene-local metres. */
    struct NavigationObstacleOverlaySurface final {
        SurfaceId id;
        NavigationWorldId world;
        NavigationGeneration topology;
        Math::Aabb bounds;
    };

    /** @brief One obstacle shape admitted to a particular surface's conservative local overlay. */
    struct NavigationObstacleOverlaySurfaceRecord final {
        SurfaceId surface;
        NavigationObstacleId obstacle;
        NavigationDynamicShape shape;
        NavigationDynamicLayerMask layers;
        NavigationObstacleOverlayChangedRegion bounds; /**< Footprint clipped to the supplied surface extent. */
    };

    /** @brief Complete projection evidence, including an empty surface overlay's exact Scene generation. */
    struct NavigationObstacleOverlaySurfaceProjection final {
        NavigationDynamicSceneBinding binding;
        NavigationDynamicRegistryRevision revision;
        SurfaceId surface;
        NavigationGeneration topology;
        std::size_t count{};
    };

    /** @brief One bounded path/movement segment wholly within an authored surface extent. */
    struct NavigationObstacleOverlaySurfaceQuery final {
        NavigationObstacleOverlaySurface surface;
        Math::Vec3 from{};
        Math::Vec3 to{};
        float radiusMeters{};
        NavigationDynamicLayerMask layers{1};
        std::size_t maximumChecks{NavigationDynamicRegistryHardLimits::Obstacles};
    };

    /**
     * @brief Tests only blockers projected onto one authored surface, retaining exact generation evidence.
     * @param snapshot One retained immutable dynamic publication.
     * @param query Surface identity/extent and bounded segment in the same canonical Scene-local frame.
     * @return Surface-keyed blocker or clear result with exact Scene binding/revision; invalid input or bound fails typed.
     * @details Both endpoints must lie within the supplied surface's planar extent; split cross-surface paths by the
     * topology owner's surface boundaries. This is logical obstruction, not a Character/Physics collision result.
     */
    [[nodiscard]] Result<NavigationObstacleOverlayProbe> ProbeNavigationObstacleOverlaySurface(
        const NavigationDynamicRegistrySnapshot &snapshot, const NavigationObstacleOverlaySurfaceQuery &query);

    /**
     * @brief Projects enabled box/cylinder records onto one authored surface without changing cooked topology.
     * @param snapshot One retained immutable dynamic publication.
     * @param surface Exact authored surface, world/topology fence, and finite canonical Scene-local extent from the same combined root.
     * @param output Caller-owned record buffer; insufficient capacity fails before writing any element.
     * @return Complete surface-keyed records in snapshot order plus exact Scene binding and revision.
     * @details This conservatively clips footprint bounds to the supplied surface extent; it cannot infer walkability
     * or make space absent from cooked topology reachable. Call separately for each distinct surface identity.
     */
    [[nodiscard]] Result<NavigationObstacleOverlaySurfaceProjection> ProjectNavigationObstacleOverlaySurface(
        const NavigationDynamicRegistrySnapshot &snapshot, const NavigationObstacleOverlaySurface &surface,
        std::span<NavigationObstacleOverlaySurfaceRecord> output);

    /**
     * @brief Projects swept old/new blocker regions onto one surface for affected-path invalidation.
     * @param previous Earlier immutable publication, or invalid for first publication.
     * @param current New immutable publication from the same Scene/world incarnation.
     * @param surface Exact authored surface and finite canonical Scene-local extent.
     * @param output Caller-owned region buffer; insufficient capacity fails without writing any element.
     * @return Complete surface-keyed regions in obstacle identity order plus the new overlay revision.
     */
    [[nodiscard]] Result<NavigationObstacleOverlaySurfaceProjection> CollectNavigationObstacleOverlaySurfaceChanges(
        const NavigationDynamicRegistrySnapshot &previous, const NavigationDynamicRegistrySnapshot &current,
        const NavigationObstacleOverlaySurface &surface, std::span<NavigationObstacleOverlayChangedRegion> output);

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
