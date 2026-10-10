#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationCoordinator.h"
#include "navigation/NavigationRuntimeTestFixtures.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Navigation {
    TEST_CASE("Compatible path partitions retain real Detour corridors through owner publication",
              "[unit][navigation][coordinator][provider]") {
        using namespace TestSupport;
        const std::array<Math::Vec3, 4> vertices{{{0, 0, 0}, {10, 0, 0}, {10, 0, 10}, {0, 0, 10}}};
        const std::array<GroundedNavigationPolygon, 1> polygons{{{.vertexIndices = {0, 1, 2, 3, 0, 0},
                                                                  .vertexCount = 4,
                                                                  .area = NavigationAreaId::Create(1).Value(),
                                                                  .surface = SurfaceId::Create(1).Value()}}};
        const std::array<NavigationAreaDescriptor, 1> areas{
            {{.id = NavigationAreaId::Create(1).Value(),
              .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
              .traversalCost = 1.0F,
              .flags = {.bits = 1}}}};
        const std::array<NavigationQueryFilterDescriptor, 1> filters{
            {{.id = NavigationFilterId::Create(1).Value(),
              .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
              .includedFlags = {},
              .excludedFlags = {},
              .costOverrides = {}}}};
        auto provider = CreateRecastDetourNavigationQueryBackend({.world = World(),
                                                                  .topology = Topology(),
                                                                  .vertices = vertices,
                                                                  .polygons = polygons,
                                                                  .maximumQueryNodes = 64,
                                                                  .maximumResultPoints = 16,
                                                                  .maximumConcurrentQueries = 1,
                                                                  .areas = areas,
                                                                  .filters = filters});
        REQUIRE(provider.HasValue());
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        REQUIRE(world.Stage(Activation(), std::move(provider).Value()).HasValue());
        REQUIRE(world.CommitAtSafePoint(Activation().scene, Activation().sceneGeneration).HasValue());
        JobSystem jobs{{.workerCount = 0}};
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, {})).Value();
        const NavigationPathCaller callers[]{{NavigationDynamicOwnerId::Create(1).Value(),
                                              NavigationDynamicOwnerGeneration::Create(1).Value()},
                                             {NavigationDynamicOwnerId::Create(2).Value(),
                                              NavigationDynamicOwnerGeneration::Create(1).Value()}};
        const NavigationOutcomeProvenance source{NavigationSnapshotToken::Create(1).Value(), World(), Topology(), 1, 1, 1, 1, 0};
        std::array<NavRequestHandle, 2> handles;
        for (std::size_t index = 0; index < handles.size(); ++index) {
            auto request = Request();
            request.start = {1, 0, 1};
            request.destination = {9, 0, 9};
            auto accepted = coordinator.Submit(world,
                                               {.caller = callers[index],
                                                .request = request,
                                                .source = source,
                                                .capabilityRevision = 1,
                                                .targetTick = 1,
                                                .deadlineTick = 10},
                                               0);
            REQUIRE(accepted.HasValue());
            handles[index] = accepted.Value();
        }
        REQUIRE(coordinator.Dispatch(1) == 2);
        REQUIRE(coordinator.Commit({Activation(), source, callers, 1}) == 0);
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(coordinator.Commit({Activation(), source, callers, 1}) == 2);
        JobId partition{};
        for (const auto handle : handles) {
            auto result = coordinator.Take(handle);
            REQUIRE(result);
            REQUIRE(result->result.HasValue());
            REQUIRE(result->result.Value().status == NavigationPathStatus::Reachable);
            REQUIRE_FALSE(result->result.Value().corridor.empty());
            REQUIRE_FALSE(result->result.Value().waypoints.empty());
            REQUIRE(result->result.Value().sourceGeneration == Topology());
            if (partition)
                REQUIRE(result->job == partition);
            partition = result->job;
            REQUIRE(partition != 0);
        }
        REQUIRE(coordinator.IsDrained());
    }
}  // namespace Horo::Navigation
