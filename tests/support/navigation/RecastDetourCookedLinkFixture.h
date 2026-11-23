#pragma once

#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationDefinition.h"
#include "navigation/IncrementalBakeFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::Navigation::TestSupport {
    /** @brief Exact cooked geometry used to exercise an authored transition between disconnected polygons. */
    [[nodiscard]] inline NavigationTileBuildResult LinkTopology(const NavigationPreparedTile &input) {
        const auto area = Id<NavigationAreaId>(1);
        NavigationTileBuildResult topology{.state = NavigationTileBuildState::Built,
                                           .key = input.tile.key.tile,
                                           .bounds = input.tile.bounds,
                                           .vertices = {{0, 0, 0}, {2, 0, 0}, {0, 0, 2}, {4, 0, 0}, {6, 0, 0}, {4, 0, 2}},
                                           .polygons = {{{0, 3}, {0, 3}, area}, {{3, 3}, {3, 3}, area}},
                                           .polygonVertexIndices = {0, 1, 2, 3, 4, 5},
                                           .polygonAdjacencies = std::vector<std::uint32_t>(6, NavMeshBoundaryAdjacency),
                                           .offMeshLinks = {{.start = {0.75F, 0, 0.75F},
                                                             .end = {4.75F, 0, 0.75F},
                                                             .radiusMeters = 1.0F,
                                                             .startPolygon = 0,
                                                             .endPolygon = 1,
                                                             .area = area,
                                                             .authoredLink = Id<NavigationLinkId>(23)}},
                                           .provenance = {{.producer = Id<NavigationSourceProducerId>(1),
                                                           .contribution = Id<NavigationSourceContributionId>(1),
                                                           .revision = Id<NavigationSourceRevision>(1),
                                                           .sourceDigest = TestSupport::Digest(10),
                                                           .polygons = {0, 2}}}};
        const auto offset = input.tile.bounds.minimum.x;
        for (auto &vertex : topology.vertices)
            vertex.x += offset;
        auto &link = topology.offMeshLinks.front();
        link.start.x += offset;
        link.end.x += offset;
        return topology;
    }

    /** @brief Drives cooked Recast admission with one exact authored tile and profile. */
    struct CookedLinkFixture final {
        const NavigationPreparedTile input = PrepareInput();
        const NavigationDefinition definition = PrepareDefinition();
        const std::array<std::shared_ptr<const NavigationCookedTile>, 1> tiles{RoundTrip(input)};

        static NavigationPreparedTile PrepareInput(const std::size_t tileIndex = 0) {
            const TestSupport::IncrementalBakeFixture bake;
            auto prepared = PrepareNavigationBakeTile(*bake.Input(), bake.Tiles()[tileIndex], bake.compatibility);
            REQUIRE(prepared.HasValue());
            return std::move(prepared).Value();
        }

        static NavigationDefinition PrepareDefinition() {
            const TestSupport::IncrementalBakeFixture bake;
            const auto areas = TestSupport::IncrementalBakeFixture::Areas();
            const auto filters = TestSupport::IncrementalBakeFixture::Filters();
            auto definition = NavigationDefinition::Create({.profiles = {bake.profile},
                                                            .areas = {areas.begin(), areas.end()},
                                                            .filters = {filters.begin(), filters.end()},
                                                            .tiles = {.tileSizeCells = 16, .maximumTiles = 8}});
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
        }

        static std::shared_ptr<const NavigationCookedTile> RoundTrip(const NavigationPreparedTile &input) {
            const auto encoded = NavigationCookedTile::Create(input, LinkTopology(input));
            REQUIRE(encoded.HasValue());
            const auto decoded = NavigationCookedTile::Decode(encoded.Value()->Bytes());
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value()->Topology().offMeshLinks == encoded.Value()->Topology().offMeshLinks);
            return decoded.Value();
        }

        RecastDetourCookedProviderCreateInfo Info() const {
            const auto &key = tiles.front()->Key();
            return {.world = Id<NavigationWorldId>(8),
                    .topology = Id<NavigationGeneration>(9),
                    .surface = {.surface = key.surface, .scope = key.scope},
                    .profile = key.profile,
                    .definition = definition,
                    .tiles = tiles,
                    .nearestPointHalfExtents = {0.25F, 1.0F, 0.25F},
                    .queryLimits = {64, 16, 80},
                    .maximumConcurrentQueries = 1,
                    .maximumOwnedBytes = 1024U * 1024U,
                    .capabilityRevision = 1};
        }

        std::unique_ptr<INavigationQueryBackend> Open() const {
            auto info = Info();
            info.allowAuthoredTraversal = true;
            auto backend = CreateRecastDetourCookedNavigationQueryBackend(info);
            CAPTURE((backend.HasError() ? backend.ErrorValue().message : "admitted"));
            REQUIRE(backend.HasValue());
            return std::move(backend).Value();
        }

        NavigationPathRequest Request() const {
            const auto info = Info();
            const auto &link = tiles.front()->Topology().offMeshLinks.front();
            return {.world = info.world,
                    .topology = info.topology,
                    .start = link.start,
                    .destination = link.end,
                    .filter = definition.Registry().Filters().front().id,
                    .requirement = {NavigationQueryKind::Path, NavigationQualityLevel::Balanced, info.queryLimits}};
        }
    };
}  // namespace Horo::Navigation::TestSupport
