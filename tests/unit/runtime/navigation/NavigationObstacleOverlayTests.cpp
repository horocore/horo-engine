#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationObstacleOverlay.h"
#include "navigation/NavigationDynamicRegistryTestFixtures.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <variant>

namespace Horo::Navigation {
    using TestSupport::Id;
    using TestSupport::MakeRegistry;
    using TestSupport::Obstacle;
    using TestSupport::OverlaySurface;
    using TestSupport::Provenance;
    using TestSupport::RequireError;
    using TestSupport::TwoObstacleOverlaySnapshot;

    TEST_CASE("Logical obstacle overlay probes one immutable Scene generation with bounded clearance",
              "[unit][navigation][headless][overlay]") {
        auto registry = MakeRegistry();
        REQUIRE(registry.StageRegisterObstacle(Obstacle(1, Provenance(), 1)).HasValue());
        REQUIRE(registry.CommitAtSafePoint(TestSupport::Activation(11, 12, 7, 9), 1).HasValue());
        const auto snapshot = registry.Snapshot().Value();
        const auto probe = ProbeNavigationObstacleOverlay(snapshot, {-3.0F, 0.0F, 0.0F}, {3.0F, 0.0F, 0.0F}, 0.0F, {1}, 1);
        REQUIRE(probe.HasValue());
        CHECK(probe.Value().blocked);
        CHECK(probe.Value().blockingObstacle == Id<NavigationObstacleId>(1));
        CHECK(probe.Value().binding == snapshot.Binding());
        CHECK(probe.Value().revision == snapshot.Revision());
        const auto otherLayer = ProbeNavigationObstacleOverlay(snapshot, {-3.0F, 0.0F, 0.0F}, {3.0F, 0.0F, 0.0F}, 0.0F, {2}, 1);
        REQUIRE(otherLayer.HasValue());
        CHECK_FALSE(otherLayer.Value().blocked);
        const auto above = ProbeNavigationObstacleOverlay(snapshot, {-3.0F, 4.0F, 0.0F}, {3.0F, 4.0F, 0.0F}, 0.0F, {1}, 1);
        REQUIRE(above.HasValue());
        CHECK_FALSE(above.Value().blocked);
        RequireError(ProbeNavigationObstacleOverlay(snapshot, {}, {}, 0.0F, {1}, 0), NavigationErrors::DynamicRegistryInvalid);
        RequireError(ProbeNavigationObstacleOverlay(snapshot, {}, {}, -1.0F, {1}, 1), NavigationErrors::DynamicRegistryInvalid);
    }

    TEST_CASE("Logical obstacle overlay reports swept changed regions without publishing partial output",
              "[unit][navigation][headless][overlay]") {
        auto registry = MakeRegistry();
        const auto activation = TestSupport::Activation(11, 12, 7, 9);
        const auto handle = registry.StageRegisterObstacle(Obstacle(1, Provenance(), 1)).Value();
        REQUIRE(registry.CommitAtSafePoint(activation, 1).HasValue());
        const auto first = registry.Snapshot().Value();
        std::array<NavigationObstacleOverlayChangedRegion, 1> changed{};
        REQUIRE(CollectNavigationObstacleOverlayChanges({}, first, changed).Value() == 1);
        CHECK(changed[0].minimumX == -1.0);
        CHECK(changed[0].maximumX == 1.0);

        REQUIRE(registry.StageUpdateObstacle(handle, first.Obstacles()[0].revision, Obstacle(1, Provenance(7, 11, 12, 21, 1, 2), 2, 3.0F))
                    .HasValue());
        REQUIRE(registry.CommitAtSafePoint(activation, 2).HasValue());
        const auto second = registry.Snapshot().Value();
        RequireError(CollectNavigationObstacleOverlayChanges(first, second, {}), NavigationErrors::DynamicRegistryCapacityExceeded);
        REQUIRE(CollectNavigationObstacleOverlayChanges(first, second, changed).Value() == 1);
        CHECK(changed[0].minimumX == -1.0);
        CHECK(changed[0].maximumX == 4.0);
        CHECK(std::get<NavigationDynamicBoxShape>(first.Obstacles()[0].shape).center.x == 0.0F);
        CHECK(second.Revision().Value() == first.Revision().Value() + 1);

        REQUIRE(registry.StageRemoveObstacle(handle, second.Obstacles()[0].revision).HasValue());
        REQUIRE(registry.CommitAtSafePoint(activation, 3).HasValue());
        const auto third = registry.Snapshot().Value();
        REQUIRE(CollectNavigationObstacleOverlayChanges(second, third, changed).Value() == 1);
        CHECK(changed[0].minimumX == 2.0);
        CHECK(changed[0].maximumX == 4.0);
    }

    TEST_CASE("Logical overlay bounds cylinder checks and preserves clear-result generation", "[unit][navigation][headless][overlay]") {
        auto registry = MakeRegistry();
        auto cylinder = Obstacle(2, Provenance(), 1);
        cylinder.shape = NavigationDynamicCylinderShape{.center = {3.0F, 0.0F, 0.0F}, .radius = 1.0F, .halfHeight = 1.0F};
        REQUIRE(registry.StageRegisterObstacle(cylinder).HasValue());
        REQUIRE(registry.CommitAtSafePoint(TestSupport::Activation(11, 12, 7, 9), 1).HasValue());
        const auto snapshot = registry.Snapshot().Value();
        const auto clear = ProbeNavigationObstacleOverlay(snapshot, {}, {1.0F, 0.0F, 0.0F}, 0.0F, {1}, 1);
        REQUIRE(clear.HasValue());
        CHECK_FALSE(clear.Value().blocked);
        CHECK(clear.Value().revision == snapshot.Revision());
        const auto blocked = ProbeNavigationObstacleOverlay(snapshot, {}, {1.0F, 0.0F, 0.0F}, 1.0F, {1}, 1);
        REQUIRE(blocked.HasValue());
        CHECK(blocked.Value().blocked);
        CHECK(blocked.Value().blockingObstacle == cylinder.id);
        const auto invalidPoint = Math::Vec3{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F};
        RequireError(ProbeNavigationObstacleOverlay(snapshot, invalidPoint, {}, 0.0F, {1}, 1), NavigationErrors::DynamicRegistryInvalid);
    }

    TEST_CASE("Logical overlay projects supported shapes onto distinct authored surfaces", "[unit][navigation][headless][overlay]") {
        const auto snapshot = TwoObstacleOverlaySnapshot();
        const auto left = OverlaySurface(10, {-2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, 2.0F});
        const auto right = OverlaySurface(20, {4.0F, 0.0F, -2.0F}, {6.0F, 0.0F, 2.0F});
        std::array<NavigationObstacleOverlaySurfaceRecord, 1> records{};
        const auto leftProjection = ProjectNavigationObstacleOverlaySurface(snapshot, left, records);
        REQUIRE(leftProjection.HasValue());
        CHECK(leftProjection.Value().binding == snapshot.Binding());
        CHECK(leftProjection.Value().revision == snapshot.Revision());
        CHECK(leftProjection.Value().surface == left.id);
        CHECK(leftProjection.Value().topology == left.topology);
        CHECK(leftProjection.Value().count == 1);
        CHECK(records[0].surface == left.id);
        CHECK(records[0].obstacle == Id<NavigationObstacleId>(1));
        CHECK(std::holds_alternative<NavigationDynamicBoxShape>(records[0].shape));
        CHECK(records[0].bounds.maximumX == 1.0);
        REQUIRE(ProjectNavigationObstacleOverlaySurface(snapshot, right, records).HasValue());
        CHECK(records[0].surface == right.id);
        CHECK(records[0].obstacle == Id<NavigationObstacleId>(2));
        CHECK(std::holds_alternative<NavigationDynamicCylinderShape>(records[0].shape));
        CHECK(records[0].bounds.minimumX == 4.0);
        RequireError(ProjectNavigationObstacleOverlaySurface(snapshot, left, {}), NavigationErrors::DynamicRegistryCapacityExceeded);
        const auto above = OverlaySurface(30, {-2.0F, 3.0F, -2.0F}, {6.0F, 3.0F, 2.0F});
        CHECK(ProjectNavigationObstacleOverlaySurface(snapshot, above, {}).Value().count == 0);
    }

    TEST_CASE("Surface-local probes retain generation and reject cross-surface segments", "[unit][navigation][headless][overlay]") {
        const auto snapshot = TwoObstacleOverlaySnapshot();
        const auto left = OverlaySurface(10, {-2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, 2.0F});
        const auto right = OverlaySurface(20, {4.0F, 0.0F, -2.0F}, {6.0F, 0.0F, 2.0F});
        const auto above = OverlaySurface(30, {-2.0F, 3.0F, -2.0F}, {6.0F, 3.0F, 2.0F});
        const NavigationObstacleOverlaySurfaceQuery leftQuery{.surface = left, .from = {-2.0F, 0.0F, 0.0F}, .to = {2.0F, 0.0F, 0.0F}};
        const auto leftProbe = ProbeNavigationObstacleOverlaySurface(snapshot, leftQuery);
        REQUIRE(leftProbe.HasValue());
        CHECK(leftProbe.Value().surface == left.id);
        CHECK(leftProbe.Value().topology == left.topology);
        CHECK(leftProbe.Value().revision == snapshot.Revision());
        CHECK(leftProbe.Value().blockingObstacle == Id<NavigationObstacleId>(1));
        const NavigationObstacleOverlaySurfaceQuery rightQuery{.surface = right, .from = {4.0F, 0.0F, 0.0F}, .to = {6.0F, 0.0F, 0.0F}};
        CHECK(ProbeNavigationObstacleOverlaySurface(snapshot, rightQuery).Value().blockingObstacle == Id<NavigationObstacleId>(2));
        const NavigationObstacleOverlaySurfaceQuery aboveQuery{.surface = above, .from = {0.0F, 3.0F, 0.0F}, .to = {1.0F, 3.0F, 0.0F}};
        const auto aboveProbe = ProbeNavigationObstacleOverlaySurface(snapshot, aboveQuery);
        REQUIRE(aboveProbe.HasValue());
        CHECK_FALSE(aboveProbe.Value().blocked);
        CHECK(aboveProbe.Value().revision == snapshot.Revision());
        auto crossSurface = leftQuery;
        crossSurface.to.x = 6.0F;
        RequireError(ProbeNavigationObstacleOverlaySurface(snapshot, crossSurface), NavigationErrors::DynamicRegistryInvalid);
        auto foreignSurface = left;
        foreignSurface.world = Id<NavigationWorldId>(8);
        std::array<NavigationObstacleOverlaySurfaceRecord, 1> records{};
        RequireError(ProjectNavigationObstacleOverlaySurface(snapshot, foreignSurface, records), NavigationErrors::DynamicRegistryInvalid);
    }

    TEST_CASE("Surface-local changed regions signal only affected surfaces and preserve complete publication",
              "[unit][navigation][headless][overlay]") {
        auto registry = MakeRegistry();
        const auto activation = TestSupport::Activation(11, 12, 7, 9);
        const auto handle = registry.StageRegisterObstacle(Obstacle(1, Provenance(), 1)).Value();
        REQUIRE(registry.CommitAtSafePoint(activation, 1).HasValue());
        const auto before = registry.Snapshot().Value();
        const auto left = OverlaySurface(10, {-2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, 2.0F});
        const auto right = OverlaySurface(20, {4.0F, 0.0F, -2.0F}, {6.0F, 0.0F, 2.0F});
        REQUIRE(registry.StageUpdateObstacle(handle, before.Obstacles()[0].revision, Obstacle(1, Provenance(7, 11, 12, 21, 1, 2), 2, 5.0F))
                    .HasValue());
        REQUIRE(registry.CommitAtSafePoint(activation, 2).HasValue());
        const auto after = registry.Snapshot().Value();
        std::array<NavigationObstacleOverlayChangedRegion, 1> changed{};
        const auto leftChange = CollectNavigationObstacleOverlaySurfaceChanges(before, after, left, changed);
        REQUIRE(leftChange.HasValue());
        CHECK(leftChange.Value().revision == after.Revision());
        CHECK(leftChange.Value().surface == left.id);
        CHECK(leftChange.Value().topology == left.topology);
        CHECK(leftChange.Value().count == 1);
        CHECK(changed[0].surface == left.id);
        CHECK(changed[0].minimumX == -1.0);
        CHECK(changed[0].maximumX == 2.0);
        const auto rightChange = CollectNavigationObstacleOverlaySurfaceChanges(before, after, right, changed);
        REQUIRE(rightChange.HasValue());
        CHECK(rightChange.Value().count == 1);
        CHECK(changed[0].surface == right.id);
        CHECK(changed[0].minimumX == 4.0);
        CHECK(changed[0].maximumX == 6.0);
        const auto crossed = OverlaySurface(25, {2.5F, 0.0F, -2.0F}, {3.5F, 0.0F, 2.0F});
        const auto crossedChange = CollectNavigationObstacleOverlaySurfaceChanges(before, after, crossed, changed);
        REQUIRE(crossedChange.HasValue());
        CHECK(crossedChange.Value().count == 1);
        CHECK(changed[0].surface == crossed.id);
        CHECK(changed[0].minimumX == 2.5);
        CHECK(changed[0].maximumX == 3.5);
        const auto untouched = OverlaySurface(30, {10.0F, 0.0F, -2.0F}, {12.0F, 0.0F, 2.0F});
        const auto untouchedChange = CollectNavigationObstacleOverlaySurfaceChanges(before, after, untouched, {});
        REQUIRE(untouchedChange.HasValue());
        CHECK(untouchedChange.Value().count == 0);
        CHECK(untouchedChange.Value().revision == after.Revision());
        const auto verticallySeparate = OverlaySurface(31, {2.5F, 3.0F, -2.0F}, {3.5F, 3.0F, 2.0F});
        CHECK(CollectNavigationObstacleOverlaySurfaceChanges(before, after, verticallySeparate, {}).Value().count == 0);
        RequireError(CollectNavigationObstacleOverlaySurfaceChanges(before, after, left, {}),
                     NavigationErrors::DynamicRegistryCapacityExceeded);
        CHECK(std::get<NavigationDynamicBoxShape>(before.Obstacles()[0].shape).center.x == 0.0F);
    }
}  // namespace Horo::Navigation
