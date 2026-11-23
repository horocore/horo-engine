#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationDefinition.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationTileArtifact.h"
#include "navigation/IncrementalBakeFixture.h"
#include "navigation/NavigationTestAssertions.h"
#include "navigation/RecastDetourCookedLinkFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using TestSupport::CookedLinkFixture;
        using TestSupport::Id;
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

    TEST_CASE("Authored single-tile links round-trip into exact native corridor boundaries",
              "[unit][navigation][provider][path][traversal]") {
        const CookedLinkFixture fixture;
        const auto &link = fixture.tiles.front()->Topology().offMeshLinks.front();
        const auto backend = fixture.Open();
        const auto route = backend->FindPath(fixture.Request(), {});
        REQUIRE(route.HasValue());
        REQUIRE(route.Value().status == NavigationPathStatus::Reachable);
        REQUIRE(route.Value().portals.size() == 1);
        const auto &portal = route.Value().portals.front();
        CHECK(portal.kind == NavigationPathPortalKind::OffMeshLink);
        CHECK(portal.link == Id<NavigationLinkId>(23));
        CHECK(portal.linkRadiusMeters == link.radiusMeters);
        CHECK(portal.left == link.start);
        CHECK(portal.right == link.end);
        const auto &waypoints = route.Value().waypoints;
        REQUIRE(waypoints.size() == 4);
        CHECK(waypoints[1].provenance.kind == NavigationPathWaypointKind::TraversalEntrance);
        CHECK(waypoints[1].provenance.portalIndex == 0);
        CHECK(waypoints[1].provenance.corridorPolygonIndex == portal.fromPolygonIndex);
        CHECK(waypoints[2].provenance.kind == NavigationPathWaypointKind::TraversalExit);
        CHECK(waypoints[2].provenance.portalIndex == 0);
        CHECK(waypoints[2].provenance.corridorPolygonIndex == portal.toPolygonIndex);
    }

    TEST_CASE("Cooked traversals obey output limits direction and clearance", "[unit][navigation][provider][path][traversal]") {
        const CookedLinkFixture fixture;
        const auto backend = fixture.Open();
        auto request = fixture.Request();
        SECTION("outer output bounds cannot produce half of a traversal") {
            request.outputLimits.maximumWaypoints = 2;
            RequireError(backend->FindPath(request, {}), NavigationErrors::CapacityExceeded);
            request.coveragePolicy = NavigationPathCoveragePolicy::AllowPartial;
            const auto partial = backend->FindPath(request, {});
            REQUIRE(partial.HasValue());
            CHECK(partial.Value().status == NavigationPathStatus::Partial);
            CHECK(partial.Value().stopReason == NavigationPathStopReason::ResultPointBudgetExceeded);
            CHECK(partial.Value().portals.empty());
            CHECK(partial.Value().corridor.size() == 1);
            CHECK(partial.Value().waypoints.size() == 1);
        }
        SECTION("reverse travel cannot use a one-way authored link") {
            std::swap(request.start, request.destination);
            const auto reverse = backend->FindPath(request, {});
            REQUIRE(reverse.HasValue());
            CHECK(reverse.Value().status == NavigationPathStatus::Unreachable);
        }
        SECTION("the entrance radius fences oversized agents") {
            request.clearanceMeters = 2.0F;
            const auto wide = backend->FindPath(request, {});
            REQUIRE(wide.HasValue());
            CHECK(wide.Value().status == NavigationPathStatus::Unreachable);
        }
    }

    TEST_CASE("A joined tile closure retains unambiguous authored traversal boundaries", "[unit][navigation][provider][path][traversal]") {
        const CookedLinkFixture fixture;
        const auto linked = CookedLinkFixture::RoundTrip(CookedLinkFixture::PrepareInput(1));
        auto info = fixture.Info();
        SECTION("A link in a later tile offsets both polygon endpoints") {
            auto plain = fixture.tiles.front()->Topology();
            plain.offMeshLinks.clear();
            const auto ground = NavigationCookedTile::Create(fixture.input, std::move(plain));
            REQUIRE(ground.HasValue());
            const std::array tiles{ground.Value(), linked};
            info.tiles = tiles;
            RequireError(CreateRecastDetourCookedNavigationQueryBackend(info), NavigationErrors::OperationUnsupported);
            info.allowAuthoredTraversal = true;
            const auto backend = CreateRecastDetourCookedNavigationQueryBackend(info);
            REQUIRE(backend.HasValue());
            auto request = fixture.Request();
            const auto &link = linked->Topology().offMeshLinks.front();
            request.start = link.start;
            request.destination = link.end;
            const auto route = backend.Value()->FindPath(request, {});
            REQUIRE(route.HasValue());
            REQUIRE(route.Value().status == NavigationPathStatus::Reachable);
            REQUIRE(route.Value().corridor.size() == 2);
            CHECK(route.Value().corridor[0].provenance.polygonIndex == 2);
            CHECK(route.Value().corridor[1].provenance.polygonIndex == 3);
            REQUIRE(route.Value().portals.size() == 1);
            const auto &portal = route.Value().portals.front();
            CHECK(portal.kind == NavigationPathPortalKind::OffMeshLink);
            CHECK(portal.link == link.authoredLink);
            CHECK(portal.left == link.start);
            CHECK(portal.right == link.end);
        }
        SECTION("Two valid tiles cannot publish the same link identity") {
            info.allowAuthoredTraversal = true;
            const std::array tiles{fixture.tiles.front(), linked};
            info.tiles = tiles;
            RequireError(CreateRecastDetourCookedNavigationQueryBackend(info), NavigationErrors::DescriptorConflict);
        }
    }

    TEST_CASE("Cooked traversal admission requires opt-in and unambiguous identity", "[unit][navigation][provider][path][traversal]") {
        const CookedLinkFixture fixture;
        auto info = fixture.Info();
        RequireError(CreateRecastDetourCookedNavigationQueryBackend(info), NavigationErrors::OperationUnsupported);
        info.allowAuthoredTraversal = true;
        const auto &tile = fixture.tiles.front();
        SECTION("authored arcs cannot mask ordinary shared polygon edges") {
            auto shared = tile->Topology();
            shared.vertices = {{0, 0, 0}, {2, 0, 0}, {0, 0, 2}, {2, 0, 2}};
            shared.polygonVertexIndices = {0, 1, 2, 1, 3, 2};
            shared.polygonAdjacencies[1] = 1;
            shared.polygonAdjacencies[5] = 0;
            shared.offMeshLinks.front().end = {1.25F, 0, 1.25F};
            const auto overlap = NavigationCookedTile::Create(fixture.input, std::move(shared));
            REQUIRE(overlap.HasValue());
            const std::array tiles{overlap.Value()};
            info.tiles = tiles;
            RequireError(CreateRecastDetourCookedNavigationQueryBackend(info), NavigationErrors::OperationUnsupported);
        }
        SECTION("legacy rows cannot issue traversal authority") {
            auto legacyTopology = tile->Topology();
            legacyTopology.offMeshLinks.front().authoredLink = {};
            const auto legacy = NavigationCookedTile::Create(fixture.input, std::move(legacyTopology));
            REQUIRE(legacy.HasValue());
            const auto restored = NavigationCookedTile::Decode(legacy.Value()->Bytes());
            REQUIRE(restored.HasValue());
            CHECK_FALSE(restored.Value()->Topology().offMeshLinks.front().authoredLink.IsValid());
            const std::array tiles{restored.Value()};
            info.tiles = tiles;
            RequireError(CreateRecastDetourCookedNavigationQueryBackend(info), NavigationErrors::OperationUnsupported);
        }
    }

}  // namespace Horo::Navigation
