#pragma once

#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "navigation/NavigationRuntimeTestFixtures.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Navigation::TestSupport {
    inline std::unique_ptr<INavigationQueryBackend> MakeGroundedProvider(const NavigationWorldId world = World(),
                                                                         const NavigationGeneration topology = Topology()) {
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
        auto provider = CreateRecastDetourNavigationQueryBackend({.world = world,
                                                                  .topology = topology,
                                                                  .vertices = vertices,
                                                                  .polygons = polygons,
                                                                  .maximumQueryNodes = 64,
                                                                  .maximumResultPoints = 16,
                                                                  .maximumConcurrentQueries = 1,
                                                                  .areas = areas,
                                                                  .filters = filters});
        REQUIRE(provider.HasValue());
        return std::move(provider).Value();
    }
}  // namespace Horo::Navigation::TestSupport
