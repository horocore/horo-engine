#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavigationProviderContract.h"
#include "navigation/NavigationTestAssertions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using TestSupport::RequireError;

        struct SquareTopology final {
            std::array<Math::Vec3, 4> vertices{{{0.0F, 0.0F, 0.0F}, {10.0F, 0.0F, 0.0F}, {10.0F, 0.0F, 10.0F}, {0.0F, 0.0F, 10.0F}}};
            std::array<GroundedNavigationPolygon, 2> polygons{{
                {
                    .vertexIndices = {0, 1, 2, 0, 0, 0},
                    .vertexCount = 3,
                    .area = NavigationAreaId::Create(1).Value(),
                    .surface = SurfaceId::Create(101).Value(),
                },
                {
                    .vertexIndices = {0, 2, 3, 0, 0, 0},
                    .vertexCount = 3,
                    .area = NavigationAreaId::Create(2).Value(),
                    .surface = SurfaceId::Create(102).Value(),
                },
            }};
            std::array<NavigationAreaDescriptor, 2> areas{{
                {.id = NavigationAreaId::Create(1).Value(),
                 .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
                 .traversalCost = 1.0F,
                 .flags = {.bits = 1}},
                {.id = NavigationAreaId::Create(2).Value(),
                 .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
                 .traversalCost = 1.0F,
                 .flags = {.bits = 2}},
            }};
            std::array<NavigationQueryFilterDescriptor, 3> filters{{
                {.id = NavigationFilterId::Create(1).Value(),
                 .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
                 .includedFlags = {},
                 .excludedFlags = {},
                 .costOverrides = {}},
                {.id = NavigationFilterId::Create(2).Value(),
                 .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
                 .includedFlags = {},
                 .excludedFlags = {.bits = 2},
                 .costOverrides = {}},
                {.id = NavigationFilterId::Create(3).Value(),
                 .source = {.kind = NavigationDescriptorSourceKind::Project, .id = NavigationDescriptorSourceId::Create(1).Value()},
                 .includedFlags = {},
                 .excludedFlags = {},
                 .costOverrides = {{.area = NavigationAreaId::Create(2).Value(), .traversalCost = 5.0F}}},
            }};
        };

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

        [[nodiscard]] RecastDetourProviderCreateInfo CreateInfo(const SquareTopology &topology) {
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

        [[nodiscard]] NavigationPathRequest Request(const RecastDetourProviderCreateInfo &info, const Math::Vec3 start = {8.0F, 0.0F, 2.0F},
                                                    const Math::Vec3 destination = {2.0F, 0.0F, 8.0F}) {
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

        [[nodiscard]] NavigationPointProjectionRequest ProjectionRequest(const RecastDetourProviderCreateInfo &info,
                                                                         const Math::Vec3 point = {5.0F, 0.0F, 5.0F},
                                                                         const std::uint32_t maximumResultPoints = 8) {
            return {
                .world = info.world,
                .topology = info.topology,
                .point = point,
                .halfExtents = {6.0F, 2.0F, 6.0F},
                .requirement = {.query = NavigationQueryKind::NearestPoint,
                                .quality = NavigationQualityLevel::Balanced,
                                .limits = {.maximumNodeExpansions = 32,
                                           .maximumResultPoints = maximumResultPoints,
                                           .maximumSearchDistanceMeters = 100.0F}},
            };
        }

        [[nodiscard]] NavigationSamplePositionRequest SampleRequest(const RecastDetourProviderCreateInfo &info,
                                                                    const Math::Vec3 center = {5.0F, 0.0F, 5.0F},
                                                                    const std::uint32_t maximumResultPoints = 8) {
            return {
                .world = info.world,
                .topology = info.topology,
                .center = center,
                .radiusMeters = 9.0F,
                .requirement = {.query = NavigationQueryKind::SamplePosition,
                                .quality = NavigationQualityLevel::Balanced,
                                .limits = {.maximumNodeExpansions = 32,
                                           .maximumResultPoints = maximumResultPoints,
                                           .maximumSearchDistanceMeters = 100.0F}},
            };
        }

        [[nodiscard]] NavigationPolygonQueryRequest PolygonRequest(const RecastDetourProviderCreateInfo &info,
                                                                   const std::uint32_t maximumResultPoints = 8) {
            return {
                .world = info.world,
                .topology = info.topology,
                .center = {5.0F, 0.0F, 5.0F},
                .halfExtents = {6.0F, 2.0F, 6.0F},
                .requirement = {.query = NavigationQueryKind::PolygonQuery,
                                .quality = NavigationQualityLevel::Balanced,
                                .limits = {.maximumNodeExpansions = 32,
                                           .maximumResultPoints = maximumResultPoints,
                                           .maximumSearchDistanceMeters = 100.0F}},
            };
        }

        [[nodiscard]] NavigationRaycastRequest RaycastRequest(const RecastDetourProviderCreateInfo &info,
                                                              const std::uint32_t maximumResultPoints = 8) {
            return {
                .world = info.world,
                .topology = info.topology,
                .start = {8.0F, 0.0F, 2.0F},
                .destination = {2.0F, 0.0F, 8.0F},
                .requirement = {.query = NavigationQueryKind::Raycast,
                                .quality = NavigationQualityLevel::Balanced,
                                .limits = {.maximumNodeExpansions = 32,
                                           .maximumResultPoints = maximumResultPoints,
                                           .maximumSearchDistanceMeters = 100.0F}},
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

    TEST_CASE("Recast Detour provider translates neutral topology and path requests", "[unit][navigation][provider]") {
        const SquareTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE_FALSE(created.HasError());
        std::unique_ptr<INavigationQueryBackend> provider = std::move(created).Value();

        const auto capabilities = provider->Capabilities();
        CHECK(ValidateNavigationProviderCapabilities(capabilities));
        CHECK(QueryNavigationSupport(capabilities, NavigationQueryKind::Path, NavigationQualityLevel::Balanced) ==
              NavigationSupport::Available);
        CHECK(QueryNavigationSupport(capabilities, NavigationQueryKind::NearestPoint, NavigationQualityLevel::Balanced) ==
              NavigationSupport::Available);
        CHECK(QueryNavigationSupport(capabilities, NavigationQueryKind::SamplePosition, NavigationQualityLevel::Balanced) ==
              NavigationSupport::Available);
        CHECK(QueryNavigationSupport(capabilities, NavigationQueryKind::Raycast, NavigationQualityLevel::Balanced) ==
              NavigationSupport::Available);
        CHECK(QueryNavigationSupport(capabilities, NavigationQueryKind::PolygonQuery, NavigationQualityLevel::Balanced) ==
              NavigationSupport::Available);
        const auto path = provider->FindPath(Request(info), {});
        REQUIRE(path.HasValue());
        REQUIRE(path.Value().points.size() >= 2);
        REQUIRE(path.Value().points.front() == Request(info).start);
        REQUIRE(path.Value().points.back() == Request(info).destination);
        CHECK(path.Value().status == NavigationPathStatus::Reachable);
        CHECK(path.Value().stopReason == NavigationPathStopReason::None);
        CHECK(path.Value().sourceGeneration == info.topology);
        CHECK(std::isfinite(path.Value().cost));
        CHECK(path.Value().cost >= 0.0F);
        REQUIRE(path.Value().lengthMeters > 0.0F);
        TestSupport::RequireNavigationProviderContract(*provider, Request(info), TestSupport::NavigationProviderFixtureOutcome::Path);

        auto exactBufferRequest = Request(info);
        exactBufferRequest.requirement.limits.maximumResultPoints = 2;
        const auto exactBufferPath = provider->FindPath(exactBufferRequest, {});
        REQUIRE(exactBufferPath.HasValue());
        CHECK(exactBufferPath.Value().status == NavigationPathStatus::Reachable);
        CHECK(exactBufferPath.Value().points.front() == exactBufferRequest.start);
        CHECK(exactBufferPath.Value().points.back() == exactBufferRequest.destination);
    }

    TEST_CASE("Recast Detour path retains a generation-scoped corridor and stable funnel provenance",
              "[unit][navigation][provider][path]") {
        const ZigZagTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();
        const auto request = Request(info, {5.0F, 0.0F, 5.0F}, {15.0F, 0.0F, 15.0F});

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

    TEST_CASE("Recast Detour reports a typed diagnostic for a portal that cannot honor clearance", "[unit][navigation][provider][path]") {
        const SquareTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();

        auto request = Request(info);
        request.clearanceMeters = 8.0F;
        RequireError(provider->FindPath(request, {}), NavigationErrors::PathPortalDegenerate);
    }

    TEST_CASE("Recast Detour path output limits bound portal and waypoint publication", "[unit][navigation][provider][path]") {
        const ZigZagTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();

        auto boundedWaypoints = Request(info, {5.0F, 0.0F, 5.0F}, {15.0F, 0.0F, 15.0F});
        boundedWaypoints.coveragePolicy = NavigationPathCoveragePolicy::AllowPartial;
        boundedWaypoints.outputLimits.maximumWaypoints = 2;
        const auto bounded = provider->FindPath(boundedWaypoints, {});
        REQUIRE(bounded.HasValue());
        CHECK(bounded.Value().waypoints.size() <= boundedWaypoints.outputLimits.maximumWaypoints);
        CHECK(bounded.Value().points.size() <= boundedWaypoints.outputLimits.maximumWaypoints);
        for (const Math::Vec3 point : bounded.Value().points)
            CHECK(Math::IsFinite(point));

        auto boundedPortals = Request(info, {5.0F, 0.0F, 5.0F}, {15.0F, 0.0F, 15.0F});
        boundedPortals.outputLimits.maximumPortals = 1;
        RequireError(provider->FindPath(boundedPortals, {}), NavigationErrors::CapacityExceeded);
    }

    TEST_CASE("Recast Detour path filters apply exclusion and traversal costs", "[unit][navigation][provider][path]") {
        const SquareTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();

        auto excluded = Request(info);
        excluded.filter = NavigationFilterId::Create(2).Value();
        excluded.coveragePolicy = NavigationPathCoveragePolicy::AllowPartial;
        const auto excludedResult = provider->FindPath(excluded, {});
        REQUIRE(excludedResult.HasValue());
        CHECK(excludedResult.Value().status == NavigationPathStatus::Unreachable);
        CHECK(excludedResult.Value().stopReason == NavigationPathStopReason::DestinationUnreachable);
        CHECK(excludedResult.Value().stopPosition == excluded.destination);
        CHECK(excludedResult.Value().sourceGeneration == excluded.topology);

        auto expensive = Request(info);
        expensive.filter = NavigationFilterId::Create(3).Value();
        const auto baseline = provider->FindPath(Request(info), {});
        const auto expensiveResult = provider->FindPath(expensive, {});
        REQUIRE(baseline.HasValue());
        REQUIRE(expensiveResult.HasValue());
        CHECK(expensiveResult.Value().status == NavigationPathStatus::Reachable);
        CHECK(expensiveResult.Value().cost > baseline.Value().cost);

        auto unknown = Request(info);
        unknown.filter = NavigationFilterId::Create(404).Value();
        RequireError(provider->FindPath(unknown, {}), NavigationErrors::FilterUnknown);
    }

    TEST_CASE("Recast Detour path search reports bounded partial progress", "[unit][navigation][provider][path]") {
        const SquareTopology topology;
        auto info = CreateInfo(topology);
        info.maximumQueryNodes = 1;
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();

        auto request = Request(info);
        request.requirement.limits.maximumNodeExpansions = info.maximumQueryNodes;
        request.coveragePolicy = NavigationPathCoveragePolicy::AllowPartial;
        const auto result = provider->FindPath(request, {});
        REQUIRE(result.HasValue());
        CHECK(result.Value().status == NavigationPathStatus::Partial);
        CHECK(result.Value().stopReason == NavigationPathStopReason::NodeBudgetExceeded);
        CHECK(result.Value().stopPolygonIndex == 0);
        CHECK(result.Value().points.size() <= request.requirement.limits.maximumResultPoints);
        CHECK(result.Value().sourceGeneration == request.topology);

        request.coveragePolicy = NavigationPathCoveragePolicy::RequireComplete;
        const auto complete = provider->FindPath(request, {});
        REQUIRE(complete.HasValue());
        CHECK(complete.Value().status == NavigationPathStatus::BudgetExceeded);
        CHECK(complete.Value().stopReason == NavigationPathStopReason::NodeBudgetExceeded);
        CHECK(complete.Value().points.empty());
    }

    TEST_CASE("Recast Detour provider rejects malformed topology without publishing partial state", "[unit][navigation][provider]") {
        SquareTopology topology;
        auto info = CreateInfo(topology);
        topology.polygons.front().vertexIndices[2] = 99;
        RequireError(CreateRecastDetourNavigationQueryBackend(info), NavigationErrors::ProviderFailed);

        SquareTopology nonFiniteTopology;
        nonFiniteTopology.vertices.front().x = std::numeric_limits<float>::quiet_NaN();
        info = CreateInfo(nonFiniteTopology);
        RequireError(CreateRecastDetourNavigationQueryBackend(info), NavigationErrors::ProviderFailed);

        const SquareTopology validTopology;
        info = CreateInfo(validTopology);
        info.maximumOwnedBytes = 1;
        RequireError(CreateRecastDetourNavigationQueryBackend(info), NavigationErrors::CapacityExceeded);

        for (const float cost : {-1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            SquareTopology invalidCostTopology;
            invalidCostTopology.areas.front().traversalCost = cost;
            RequireError(CreateRecastDetourNavigationQueryBackend(CreateInfo(invalidCostTopology)),
                         NavigationErrors::AreaDescriptorInvalid);
        }

        info = CreateInfo(validTopology);
        auto recovered = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(recovered.HasValue());
        REQUIRE(recovered.Value()->FindPath(Request(info), {}).HasValue());
    }

    TEST_CASE("Recast Detour provider fences cancellation world topology and request bounds", "[unit][navigation][provider]") {
        const SquareTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();

        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(provider->FindPath(Request(info), cancellation.Token()), NavigationErrors::QueryCancelled);

        auto request = Request(info);
        request.world = NavigationWorldId::Create(8).Value();
        RequireError(provider->FindPath(request, {}), NavigationErrors::InvalidWorld);
        request = Request(info);
        request.topology = NavigationGeneration::Create(12).Value();
        RequireError(provider->FindPath(request, {}), NavigationErrors::StaleSnapshot);
        request = Request(info);
        request.requirement.limits.maximumResultPoints = info.maximumResultPoints + 1U;
        RequireError(provider->FindPath(request, {}), NavigationErrors::QueryLimitExceeded);
    }

    TEST_CASE("Recast Detour spatial primitives preserve metadata and deterministic tie order", "[unit][navigation][provider]") {
        const SquareTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();

        const auto firstProjection = provider->ProjectPoint(ProjectionRequest(info), {});
        const auto secondProjection = provider->ProjectPoint(ProjectionRequest(info), {});
        REQUIRE(firstProjection.HasValue());
        REQUIRE(secondProjection.HasValue());
        REQUIRE(firstProjection.Value().hit == secondProjection.Value().hit);
        CHECK(firstProjection.Value().hit.provenance.polygonIndex == 0);
        CHECK(firstProjection.Value().hit.surface == topology.polygons[0].surface);
        CHECK(firstProjection.Value().hit.area == topology.polygons[0].area);
        CHECK(std::isfinite(firstProjection.Value().hit.position.x));
        CHECK(std::isfinite(firstProjection.Value().hit.position.y));
        CHECK(std::isfinite(firstProjection.Value().hit.position.z));
        CHECK(firstProjection.Value().hit.distanceMeters < 0.2F);
        CHECK((firstProjection.Value().hit.normal == Math::Vec3{0.0F, 1.0F, 0.0F}));

        const auto polygonQuery = provider->QueryPolygons(PolygonRequest(info), {});
        REQUIRE(polygonQuery.HasValue());
        REQUIRE(polygonQuery.Value().polygons.size() == 2);
        CHECK(polygonQuery.Value().polygons[0].provenance.polygonIndex == 0);
        CHECK(polygonQuery.Value().polygons[1].provenance.polygonIndex == 1);

        const auto samples = provider->SamplePosition(SampleRequest(info), {});
        REQUIRE(samples.HasValue());
        REQUIRE(samples.Value().samples.size() == 2);
        CHECK(samples.Value().samples[0].provenance.polygonIndex == 0);
        CHECK(samples.Value().samples[1].provenance.polygonIndex == 1);

        const auto raycast = provider->Raycast(RaycastRequest(info), {});
        REQUIRE(raycast.HasValue());
        CHECK_FALSE(raycast.Value().blocked);
        CHECK_FALSE(raycast.Value().truncated);
        REQUIRE(raycast.Value().traversedPolygons.size() == 2);
        CHECK(raycast.Value().traversedPolygons[0].polygonIndex == 0);
        CHECK(raycast.Value().traversedPolygons[1].polygonIndex == 1);
        CHECK(raycast.Value().hit.position == RaycastRequest(info).destination);

        auto boundaryRay = RaycastRequest(info);
        boundaryRay.destination = {20.0F, 0.0F, 2.0F};
        const auto boundaryResult = provider->Raycast(boundaryRay, {});
        REQUIRE(boundaryResult.HasValue());
        REQUIRE(boundaryResult.Value().blocked);
        CHECK(boundaryResult.Value().hit.position.x < boundaryRay.destination.x);
        CHECK(boundaryResult.Value().hit.provenance.polygonIndex == 0);
    }

    TEST_CASE("Recast Detour spatial results obey caller and provider bounds", "[unit][navigation][provider]") {
        const SquareTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();

        const auto polygons = provider->QueryPolygons(PolygonRequest(info, 1), {});
        REQUIRE(polygons.HasValue());
        REQUIRE(polygons.Value().polygons.size() == 1);
        REQUIRE(polygons.Value().truncated);
        CHECK(polygons.Value().polygons.front().provenance.polygonIndex == 0);

        const auto samples = provider->SamplePosition(SampleRequest(info, {5.0F, 0.0F, 5.0F}, 1), {});
        REQUIRE(samples.HasValue());
        REQUIRE(samples.Value().samples.size() == 1);
        REQUIRE(samples.Value().truncated);
        CHECK(samples.Value().samples.front().provenance.polygonIndex == 0);

        const auto raycast = provider->Raycast(RaycastRequest(info, 1), {});
        REQUIRE(raycast.HasValue());
        REQUIRE(raycast.Value().traversedPolygons.size() == 1);
        REQUIRE(raycast.Value().truncated);
        CHECK(raycast.Value().traversedPolygons.front().polygonIndex == 0);
    }

    TEST_CASE("Recast Detour spatial primitives reject malformed input before provider work", "[unit][navigation][provider]") {
        const SquareTopology topology;
        const auto info = CreateInfo(topology);
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();

        RequireError(provider->ProjectPoint(ProjectionRequest(info, {nan, 0.0F, 0.0F}), {}), NavigationErrors::CapabilityDescriptorInvalid);
        auto sample = SampleRequest(info);
        sample.center.x = infinity;
        RequireError(provider->SamplePosition(sample, {}), NavigationErrors::CapabilityDescriptorInvalid);
        auto raycast = RaycastRequest(info);
        raycast.destination.z = nan;
        RequireError(provider->Raycast(raycast, {}), NavigationErrors::CapabilityDescriptorInvalid);
        auto polygons = PolygonRequest(info);
        polygons.halfExtents.y = nan;
        RequireError(provider->QueryPolygons(polygons, {}), NavigationErrors::CapabilityDescriptorInvalid);

        auto wrongKind = ProjectionRequest(info);
        wrongKind.requirement.query = NavigationQueryKind::Path;
        RequireError(provider->ProjectPoint(wrongKind, {}), NavigationErrors::CapabilityDescriptorInvalid);

        auto invalidWorld = ProjectionRequest(info);
        invalidWorld.world = {};
        RequireError(provider->ProjectPoint(invalidWorld, {}), NavigationErrors::InvalidWorld);
        auto staleTopology = ProjectionRequest(info);
        staleTopology.topology = NavigationGeneration::Create(12).Value();
        RequireError(provider->ProjectPoint(staleTopology, {}), NavigationErrors::StaleSnapshot);

        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(provider->ProjectPoint(ProjectionRequest(info), cancellation.Token()), NavigationErrors::QueryCancelled);
        REQUIRE(provider->ProjectPoint(ProjectionRequest(info), {}).HasValue());
    }

    TEST_CASE("Recast Detour provider construction and teardown repeat across scene reload", "[unit][navigation][provider]") {
        const SquareTopology topology;
        const auto info = CreateInfo(topology);
        for (std::size_t reload = 0; reload < 32; ++reload) {
            auto created = CreateRecastDetourNavigationQueryBackend(info);
            REQUIRE(created.HasValue());
            const auto path = created.Value()->FindPath(Request(info), {});
            REQUIRE(path.HasValue());
        }
    }

    TEST_CASE("Recast Detour query leases never share native query scratch", "[unit][navigation][provider]") {
        const SquareTopology topology;
        auto info = CreateInfo(topology);
        info.maximumConcurrentQueries = 1;
        auto created = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(created.HasValue());
        auto provider = std::move(created).Value();
        std::atomic<bool> unexpected{false};

        auto query = [&] {
            for (std::size_t iteration = 0; iteration < 500; ++iteration) {
                const auto result = provider->FindPath(Request(info), {});
                if (result.HasValue())
                    continue;
                if (result.ErrorValue().code.Value() != NavigationErrors::AdmissionRejected.code.Value())
                    unexpected.store(true, std::memory_order_relaxed);
            }
        };
        std::array<std::thread, 4> workers{std::thread{query}, std::thread{query}, std::thread{query}, std::thread{query}};
        for (std::thread &worker : workers)
            worker.join();
        REQUIRE_FALSE(unexpected.load(std::memory_order_relaxed));
    }
}  // namespace Horo::Navigation
