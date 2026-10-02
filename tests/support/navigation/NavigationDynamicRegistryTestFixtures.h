#pragma once

#include "Horo/Navigation/NavigationDynamicRegistry.h"
#include "Horo/Navigation/NavigationObstacleOverlay.h"
#include "navigation/NavigationRuntimeTestFixtures.h"
#include "navigation/NavigationTestAssertions.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

namespace Horo::Navigation::TestSupport {
    [[nodiscard]] inline NavigationDynamicProvenance Provenance(
        const std::uint64_t world = 7, const std::uint64_t scene = 11, const std::uint64_t sceneGeneration = 12,
        const std::uint64_t owner = 21, const std::uint64_t ownerGeneration = 1, const std::uint64_t sourceRevision = 1,
        const NavigationDynamicSourceKind source = NavigationDynamicSourceKind::SceneEntity, const std::uint64_t authoredModifier = 0) {
        return {
            .world = Id<NavigationWorldId>(world),
            .scene = Id<NavigationSceneRuntimeId>(scene),
            .sceneGeneration = Id<NavigationSceneGeneration>(sceneGeneration),
            .owner = Id<NavigationDynamicOwnerId>(owner),
            .ownerGeneration = Id<NavigationDynamicOwnerGeneration>(ownerGeneration),
            .sourceRevision = Id<NavigationDynamicSourceRevision>(sourceRevision),
            .source = source,
            .authoredModifier = authoredModifier == 0 ? NavigationModifierId{} : Id<NavigationModifierId>(authoredModifier),
        };
    }

    [[nodiscard]] inline NavigationObstacleDescriptor Obstacle(const std::uint64_t id, const NavigationDynamicProvenance &provenance,
                                                               const std::uint64_t updateTick, const float centerX = 0.0F) {
        return {
            .id = Id<NavigationObstacleId>(id),
            .provenance = provenance,
            .shape = NavigationDynamicBoxShape{.center = {centerX, 0.0F, 0.0F}, .halfExtents = {1.0F, 1.0F, 1.0F}},
            .layers = {.bits = 1},
            .priority = 0,
            .updateTick = updateTick,
            .enabled = true,
        };
    }

    [[nodiscard]] inline NavigationDynamicRegistry MakeRegistry(const NavigationDynamicRegistryLimits &limits = {}) {
        auto result = NavigationDynamicRegistry::Create(limits);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    [[nodiscard]] inline NavigationObstacleOverlaySurface OverlaySurface(const std::uint64_t id, const Math::Vec3 minimum,
                                                                         const Math::Vec3 maximum) {
        return {.id = Id<SurfaceId>(id),
                .world = Id<NavigationWorldId>(7),
                .topology = Id<NavigationGeneration>(9),
                .bounds = {minimum, maximum}};
    }

    [[nodiscard]] inline NavigationDynamicRegistrySnapshot TwoObstacleOverlaySnapshot() {
        auto registry = MakeRegistry();
        REQUIRE(registry.StageRegisterObstacle(Obstacle(1, Provenance(), 1)).HasValue());
        auto cylinder = Obstacle(2, Provenance(7, 11, 12, 22), 1, 5.0F);
        cylinder.shape = NavigationDynamicCylinderShape{.center = {5.0F, 0.0F, 0.0F}, .radius = 1.0F, .halfHeight = 1.0F};
        REQUIRE(registry.StageRegisterObstacle(cylinder).HasValue());
        REQUIRE(registry.CommitAtSafePoint(Activation(11, 12, 7, 9), 1).HasValue());
        return registry.Snapshot().Value();
    }
}  // namespace Horo::Navigation::TestSupport
