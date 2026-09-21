#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavigationProviderContract.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
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
                    .area = NavigationAreaId::Create(1).Value(),
                    .surface = SurfaceId::Create(102).Value(),
                },
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
            };
        }

        [[nodiscard]] NavigationPathRequest Request(const RecastDetourProviderCreateInfo &info) {
            return {
                .world = info.world,
                .topology = info.topology,
                .start = {8.0F, 0.0F, 2.0F},
                .destination = {2.0F, 0.0F, 8.0F},
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
        REQUIRE(path.Value().lengthMeters > 0.0F);
        TestSupport::RequireNavigationProviderContract(*provider, Request(info), TestSupport::NavigationProviderFixtureOutcome::Path);
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
