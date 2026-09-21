#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavigationTestAssertions.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Digest;
        using TestSupport::Id;
        using TestSupport::RequireError;

        [[nodiscard]] NavigationTileBuildRequest CreateRequest(const std::span<const NavigationTileBuildTriangle> triangles) {
            return {
                .key = {.x = 0, .z = 0, .layer = 0},
                .bounds = {.minimum = {0.0F, -1.0F, 0.0F}, .maximum = {10.0F, 3.0F, 10.0F}},
                .tileSizeMeters = 10.0F,
                .buildGeometry =
                    {
                        .radiusMeters = 0.3F,
                        .heightMeters = 1.8F,
                        .maxSlopeDegrees = 45.0F,
                        .stepHeightMeters = 0.4F,
                        .cellSizeMeters = 0.3F,
                        .cellHeightMeters = 0.2F,
                        .minimumRegionSizeMeters = 0.0F,
                    },
                .triangles = triangles,
            };
        }

        [[nodiscard]] std::vector<NavigationTileBuildTriangle> FloorTriangles() {
            return {
                {
                    .vertices = {{{0.0F, 0.0F, 0.0F}, {10.0F, 0.0F, 10.0F}, {10.0F, 0.0F, 0.0F}}},
                    .area = Id<NavigationAreaId>(1),
                    .traversalCost = 1.0F,
                    .materialSlot = {.value = 7},
                    .provenance =
                        {
                            .kind = NavigationSourceProducerKind::StaticCollider,
                            .producer = Id<NavigationSourceProducerId>(1),
                            .contribution = Id<NavigationSourceContributionId>(1),
                            .revision = Id<NavigationSourceRevision>(1),
                            .contentDigest = Digest(10),
                            .sourceTriangleIndex = 0,
                        },
                },
                {
                    .vertices = {{{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 10.0F}, {10.0F, 0.0F, 10.0F}}},
                    .area = Id<NavigationAreaId>(1),
                    .traversalCost = 1.0F,
                    .materialSlot = {.value = 7},
                    .provenance =
                        {
                            .kind = NavigationSourceProducerKind::StaticCollider,
                            .producer = Id<NavigationSourceProducerId>(1),
                            .contribution = Id<NavigationSourceContributionId>(1),
                            .revision = Id<NavigationSourceRevision>(1),
                            .contentDigest = Digest(10),
                            .sourceTriangleIndex = 1,
                        },
                },
            };
        }

        void RequireEquivalent(const NavigationTileBuildResult &left, const NavigationTileBuildResult &right) {
            REQUIRE(left.state == right.state);
            REQUIRE(left.key == right.key);
            REQUIRE(left.bounds.minimum == right.bounds.minimum);
            REQUIRE(left.bounds.maximum == right.bounds.maximum);
            REQUIRE(left.statistics == right.statistics);
            REQUIRE(left.vertices == right.vertices);
            REQUIRE(left.polygons == right.polygons);
            REQUIRE(left.polygonVertexIndices == right.polygonVertexIndices);
            REQUIRE(left.polygonAdjacencies == right.polygonAdjacencies);
            REQUIRE(left.offMeshLinks == right.offMeshLinks);
            REQUIRE(left.provenance == right.provenance);
            REQUIRE(left.warnings == right.warnings);
        }
    }  // namespace

    TEST_CASE("Recast tile builder produces deterministic grounded polygons and provider-neutral statistics",
              "[unit][navigation][provider][tile_build]") {
        auto triangles = FloorTriangles();
        auto builder = CreateRecastDetourNavigationMeshBuilder();
        REQUIRE(builder.HasValue());

        auto first = builder.Value()->BuildTile(CreateRequest(triangles), {});
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().state == NavigationTileBuildState::Built);
        REQUIRE_FALSE(first.Value().polygons.empty());
        REQUIRE_FALSE(first.Value().vertices.empty());
        REQUIRE(first.Value().statistics.inputTriangleCount == triangles.size());
        REQUIRE(first.Value().statistics.walkableTriangleCount == triangles.size());
        REQUIRE(first.Value().statistics.regionCount > 0);
        REQUIRE(first.Value().statistics.contourCount > 0);
        REQUIRE(first.Value().statistics.outputOwnedBytes > 0);
        REQUIRE(first.Value().provenance.size() == 1);
        REQUIRE(first.Value().offMeshLinks.empty());

        std::ranges::reverse(triangles);
        auto second = builder.Value()->BuildTile(CreateRequest(triangles), {});
        REQUIRE(second.HasValue());
        RequireEquivalent(first.Value(), second.Value());
    }

    TEST_CASE("Recast tile builder distinguishes valid empty tiles from steep or excluded geometry",
              "[unit][navigation][provider][tile_build]") {
        auto builder = CreateRecastDetourNavigationMeshBuilder();
        REQUIRE(builder.HasValue());

        const auto floorTriangles = FloorTriangles();
        auto emptyTileRequest = CreateRequest(floorTriangles);
        emptyTileRequest.key.x = 1;
        emptyTileRequest.bounds = {.minimum = {10.0F, -1.0F, 0.0F}, .maximum = {20.0F, 3.0F, 10.0F}};
        const auto emptyTile = builder.Value()->BuildTile(emptyTileRequest, {});
        REQUIRE(emptyTile.HasValue());
        REQUIRE(emptyTile.Value().IsEmpty());
        REQUIRE(emptyTile.Value().polygons.empty());

        const std::array steepTriangles{
            NavigationTileBuildTriangle{
                .vertices = {{{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 10.0F}, {0.0F, 3.0F, 0.0F}}},
                .area = Id<NavigationAreaId>(1),
                .traversalCost = 1.0F,
                .materialSlot = {.value = 7},
                .provenance = FloorTriangles().front().provenance,
            },
        };
        const auto steepTile = builder.Value()->BuildTile(CreateRequest(steepTriangles), {});
        REQUIRE(steepTile.HasValue());
        REQUIRE(steepTile.Value().IsEmpty());
        REQUIRE_FALSE(steepTile.Value().warnings.empty());

        auto excludedRequest = CreateRequest(floorTriangles);
        const NavigationTileBuildModifier modifier{
            .id = Id<NavigationModifierId>(9),
            .surface = Id<SurfaceId>(3),
            .profile = Id<NavigationAgentProfileId>(4),
            .mode = NavigationBakeModifierMode::Exclude,
            .area = Id<NavigationAreaId>(1),
            .canonicalBounds = {.minimum = {0.0F, -1.0F, 0.0F}, .maximum = {10.0F, 3.0F, 10.0F}},
        };
        excludedRequest.modifiers = std::span<const NavigationTileBuildModifier>(&modifier, 1);
        const auto excludedTile = builder.Value()->BuildTile(excludedRequest, {});
        REQUIRE(excludedTile.HasValue());
        REQUIRE(excludedTile.Value().IsEmpty());
    }

    TEST_CASE("Recast tile builder applies area reassignment to generated polygons", "[unit][navigation][provider][tile_build]") {
        auto builder = CreateRecastDetourNavigationMeshBuilder();
        REQUIRE(builder.HasValue());
        const auto floorTriangles = FloorTriangles();
        auto reassignedRequest = CreateRequest(floorTriangles);
        const NavigationTileBuildModifier reassignment{
            .id = Id<NavigationModifierId>(10),
            .surface = Id<SurfaceId>(3),
            .profile = Id<NavigationAgentProfileId>(4),
            .mode = NavigationBakeModifierMode::AssignArea,
            .area = Id<NavigationAreaId>(2),
            .canonicalBounds = {.minimum = {0.0F, -1.0F, 0.0F}, .maximum = {10.0F, 3.0F, 10.0F}},
        };
        reassignedRequest.modifiers = std::span<const NavigationTileBuildModifier>(&reassignment, 1);
        const auto reassignedTile = builder.Value()->BuildTile(reassignedRequest, {});
        REQUIRE(reassignedTile.HasValue());
        REQUIRE(reassignedTile.Value().state == NavigationTileBuildState::Built);
        REQUIRE(std::ranges::all_of(reassignedTile.Value().polygons, [](const NavMeshPolygon &polygon) {
            return polygon.area == Id<NavigationAreaId>(2);
        }));
        REQUIRE_FALSE(reassignedTile.Value().provenance.empty());
    }

    TEST_CASE("Recast tile builder rejects over-budget output without affecting a later tile build",
              "[unit][navigation][provider][tile_build][capacity]") {
        auto triangles = FloorTriangles();
        auto builder = CreateRecastDetourNavigationMeshBuilder();
        REQUIRE(builder.HasValue());

        auto overBudgetRequest = CreateRequest(triangles);
        overBudgetRequest.limits.maximumVertices = 1;
        RequireError(builder.Value()->BuildTile(overBudgetRequest, {}), NavigationErrors::CapacityExceeded);

        const auto recovered = builder.Value()->BuildTile(CreateRequest(triangles), {});
        REQUIRE(recovered.HasValue());
        REQUIRE(recovered.Value().state == NavigationTileBuildState::Built);
    }

    TEST_CASE("Recast tile builder enforces cancellation and malformed tile contracts",
              "[unit][navigation][provider][tile_build][validation]") {
        auto triangles = FloorTriangles();
        auto builder = CreateRecastDetourNavigationMeshBuilder();
        REQUIRE(builder.HasValue());

        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(builder.Value()->BuildTile(CreateRequest(triangles), cancellation.Token()), NavigationErrors::BakeInputCancelled);

        auto malformed = CreateRequest(triangles);
        malformed.bounds.maximum.x = 11.0F;
        RequireError(builder.Value()->BuildTile(malformed, {}), NavigationErrors::BakeInputInvalid);

        malformed = CreateRequest(triangles);
        malformed.limits.maximumOwnedBytes = 1;
        RequireError(builder.Value()->BuildTile(malformed, {}), NavigationErrors::CapacityExceeded);

        malformed = CreateRequest(triangles);
        malformed.limits.maximumPolygons = NavigationTileBuildLimits::MaximumPolygons + 1;
        RequireError(builder.Value()->BuildTile(malformed, {}), NavigationErrors::BakeInputInvalid);

        malformed = CreateRequest(triangles);
        malformed.limits.maximumOffMeshLinks = 0;
        RequireError(builder.Value()->BuildTile(malformed, {}), NavigationErrors::BakeInputInvalid);

        malformed = CreateRequest(triangles);
        malformed.limits.maximumWorkUnits = 1;
        RequireError(builder.Value()->BuildTile(malformed, {}), NavigationErrors::CapacityExceeded);
    }
}  // namespace Horo::Navigation
