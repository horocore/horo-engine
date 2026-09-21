#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavigationTestAssertions.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>

namespace Horo::Navigation {
    namespace {
        using TestSupport::RequireError;

        struct ZigZagTopology final {
            std::array<Math::Vec3, 8> vertices{{{0.0F, 0.0F, 0.0F},
                                                {10.0F, 0.0F, 0.0F},
                                                {10.0F, 0.0F, 10.0F},
                                                {0.0F, 0.0F, 10.0F},
                                                {20.0F, 0.0F, 0.0F},
                                                {20.0F, 0.0F, 10.0F},
                                                {20.0F, 0.0F, 20.0F},
                                                {10.0F, 0.0F, 20.0F}}};
            std::array<GroundedNavigationPolygon, 3> polygons{{
                {
                    .vertexIndices = {0, 1, 2, 3, 0, 0},
                    .vertexCount = 4,
                    .area = NavigationAreaId::Create(1).Value(),
                    .surface = SurfaceId::Create(101).Value(),
                },
                {
                    .vertexIndices = {1, 4, 5, 2, 0, 0},
                    .vertexCount = 4,
                    .area = NavigationAreaId::Create(2).Value(),
                    .surface = SurfaceId::Create(102).Value(),
                },
                {
                    .vertexIndices = {2, 5, 6, 7, 0, 0},
                    .vertexCount = 4,
                    .area = NavigationAreaId::Create(3).Value(),
                    .surface = SurfaceId::Create(103).Value(),
                },
            }};
            std::array<NavigationAreaDescriptor, 3> areas{{
                {.id = NavigationAreaId::Create(1).Value(),
                 .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
                 .traversalCost = 1.0F,
                 .flags = {.bits = 1}},
                {.id = NavigationAreaId::Create(2).Value(),
                 .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
                 .traversalCost = 1.0F,
                 .flags = {.bits = 2}},
                {.id = NavigationAreaId::Create(3).Value(),
                 .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
                 .traversalCost = 1.0F,
                 .flags = {.bits = 4}},
            }};
            std::array<NavigationQueryFilterDescriptor, 1> filters{{
                {.id = NavigationFilterId::Create(1).Value(),
                 .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
                 .includedFlags = {},
                 .excludedFlags = {},
                 .costOverrides = {}},
            }};
        };

        [[nodiscard]] RecastDetourProviderCreateInfo CreateInfo(const ZigZagTopology &topology) {
            return {
                .world = NavigationWorldId::Create(7).Value(),
                .topology = NavigationGeneration::Create(11).Value(),
                .vertices = topology.vertices,
                .polygons = topology.polygons,
                .maximumQueryNodes = 64,
                .maximumResultPoints = 16,
                .maximumConcurrentQueries = 2,
                .areas = topology.areas,
                .filters = topology.filters,
            };
        }

        [[nodiscard]] NavigationPathRequest Request(const RecastDetourProviderCreateInfo &info, const Math::Vec3 start = {5.0F, 0.0F, 5.0F},
                                                    const Math::Vec3 destination = {15.0F, 0.0F, 15.0F}) {
            return {
                .world = info.world,
                .topology = info.topology,
                .start = start,
                .destination = destination,
                .filter = NavigationFilterId::Create(1).Value(),
                .coveragePolicy = NavigationPathCoveragePolicy::RequireComplete,
                .requirement =
                    {
                        .query = NavigationQueryKind::Path,
                        .quality = NavigationQualityLevel::Balanced,
                        .limits = {.maximumNodeExpansions = 32, .maximumResultPoints = 8, .maximumSearchDistanceMeters = 100.0F},
                    },
            };
        }

        [[nodiscard]] bool IsInsidePolygonXZ(const Math::Vec3 point, const GroundedNavigationPolygon &polygon,
                                             const std::span<const Math::Vec3> vertices, const double tolerance = 0.01) {
            for (std::uint8_t edge = 0; edge < polygon.vertexCount; ++edge) {
                const Math::Vec3 first = vertices[polygon.vertexIndices[edge]];
                const Math::Vec3 second = vertices[polygon.vertexIndices[(edge + 1U) % polygon.vertexCount]];
                const Math::Vec3 edgeVector = second - first;
                const Math::Vec3 pointVector = point - first;
                const double cross =
                    (static_cast<double>(edgeVector.x) * pointVector.z) - (static_cast<double>(edgeVector.z) * pointVector.x);
                if (cross < -tolerance)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool IsInsideCorridor(const NavigationPath &path, const ZigZagTopology &topology, const Math::Vec3 point) {
            return std::ranges::any_of(path.corridor, [&topology, point](const NavigationPathCorridorPolygon &polygon) {
                const auto polygonIndex = polygon.provenance.polygonIndex;
                return polygonIndex < topology.polygons.size() &&
                       IsInsidePolygonXZ(point, topology.polygons[polygonIndex], topology.vertices);
            });
        }
    }  // namespace

    TEST_CASE("Recast Detour path retains a generation-scoped corridor and stable funnel provenance",
              "[unit][navigation][provider][path]") {
        const ZigZagTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();
        const auto request = Request(info);

        const auto first = provider->FindPath(request, {});
        const auto second = provider->FindPath(request, {});
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().status == NavigationPathStatus::Reachable);
        REQUIRE(first.Value().corridor.size() == 3);
        REQUIRE(first.Value().portals.size() == 2);
        REQUIRE(first.Value().waypoints.size() == first.Value().points.size());
        REQUIRE(first.Value().waypoints.size() <= request.requirement.limits.maximumResultPoints);
        CHECK(first.Value().corridor == second.Value().corridor);
        CHECK(first.Value().portals == second.Value().portals);
        CHECK(first.Value().waypoints == second.Value().waypoints);
        CHECK(first.Value().points == second.Value().points);

        for (std::size_t index = 0; index < first.Value().corridor.size(); ++index) {
            CHECK(first.Value().corridor[index].provenance.world == request.world);
            CHECK(first.Value().corridor[index].provenance.topology == request.topology);
            CHECK(first.Value().corridor[index].provenance.polygonIndex == index);
        }
        for (const NavigationPathPortal &portal : first.Value().portals) {
            CHECK(portal.widthMeters > 0.0F);
            CHECK(std::isfinite(portal.left.x));
            CHECK(std::isfinite(portal.left.z));
            CHECK(std::isfinite(portal.right.x));
            CHECK(std::isfinite(portal.right.z));
        }
        REQUIRE(std::ranges::any_of(first.Value().waypoints, [](const NavigationPathWaypoint &waypoint) {
            return waypoint.provenance.kind == NavigationPathWaypointKind::PortalCorner;
        }));
        for (const NavigationPathWaypoint &waypoint : first.Value().waypoints) {
            CHECK(Math::IsFinite(waypoint.position));
            CHECK(IsInsideCorridor(first.Value(), topology, waypoint.position));
            CHECK(waypoint.provenance.polygon.world == request.world);
            CHECK(waypoint.provenance.polygon.topology == request.topology);
            if (waypoint.provenance.kind == NavigationPathWaypointKind::PortalCorner) {
                CHECK(waypoint.provenance.portalIndex < first.Value().portals.size());
                CHECK(waypoint.provenance.vertexIndex != NavigationPathNoVertex);
            }
        }
    }

    TEST_CASE("Recast Detour path output limits bound portal and waypoint publication", "[unit][navigation][provider][path]") {
        const ZigZagTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();

        auto boundedWaypoints = Request(info);
        boundedWaypoints.coveragePolicy = NavigationPathCoveragePolicy::AllowPartial;
        boundedWaypoints.outputLimits.maximumWaypoints = 2;
        const auto bounded = provider->FindPath(boundedWaypoints, {});
        REQUIRE(bounded.HasValue());
        CHECK(bounded.Value().waypoints.size() <= boundedWaypoints.outputLimits.maximumWaypoints);
        CHECK(bounded.Value().points.size() <= boundedWaypoints.outputLimits.maximumWaypoints);
        for (const Math::Vec3 point : bounded.Value().points)
            CHECK(Math::IsFinite(point));

        auto boundedPortals = Request(info);
        boundedPortals.outputLimits.maximumPortals = 1;
        RequireError(provider->FindPath(boundedPortals, {}), NavigationErrors::CapacityExceeded);
    }
}  // namespace Horo::Navigation
