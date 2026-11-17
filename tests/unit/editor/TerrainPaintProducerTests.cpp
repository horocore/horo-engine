#include "terrain/TerrainPaintProducer.h"

#include <catch2/catch_test_macros.hpp>
#include <numeric>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    template <class T> T Id(std::uint8_t value) {
        Terrain::SerializedTerrainIdentity bytes{};
        bytes.back() = value;
        return T::Create(bytes).Value();
    }

    Terrain::TerrainCanonicalSource Source() {
        Terrain::TerrainCanonicalSource value;
        value.dataset = Id<Terrain::TerrainDatasetId>(1);
        value.sourceAsset = Assets::AssetId::FromBytes(value.dataset.Bytes());
        value.revision = Terrain::TerrainSourceRevision::Create(1).Value();
        value.capability = Terrain::TerrainCapabilityRevision::Create(1).Value();
        value.width = 9;
        value.height = 9;
        value.layerCount = 16;
        value.heightsMeters.resize(81);
        value.weights.resize(81 * 16);
        value.holes.resize(81);
        for (std::size_t i = 0; i < 81; ++i)
            value.weights[i * 16] = 65535;
        return value;
    }

    Terrain::TerrainAuthoringLayerBinding Binding() {
        Terrain::TerrainAuthoringLayerBinding value{.asset = Assets::AssetId::FromBytes(Id<Terrain::TerrainLayerSetId>(2).Bytes()),
                                                    .id = Id<Terrain::TerrainLayerSetId>(2),
                                                    .revision = Terrain::TerrainContentRevision::Create(3).Value(),
                                                    .digest = {{1}}};
        for (std::uint8_t i = 0; i < 16; ++i)
            value.order[i] = Id<Terrain::TerrainLayerId>(i + 1);
        return value;
    }

    struct Fixture final {
        TerrainAuthoringDocument document = std::move(TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(1).Value(), Source(),
                                                                                     4, TerrainAuthoringCapability::Edit))
                                                .Value();
        Terrain::TerrainAuthoringLayerBinding binding = Binding();
        TerrainPaintBrush brush;
        std::array<TerrainSculptStamp, 1> stamps{TerrainSculptStamp{4, 4}};

        Fixture() {
            brush.targets[0] = {binding.order[15], 1};
            brush.targetCount = 1;
        }

        TerrainPaintFence Fence() const {
            return {document.Fence(), binding.id, binding.revision, binding.digest};
        }

        Result<TerrainEditOperation> Produce(const TerrainSculptLimits &limits = {}, const CancellationToken &token = {}) const {
            return ProduceTerrainPaint(document.Source(), binding, Fence(), TerrainEditOperationId::Create(1).Value(), brush, stamps,
                                       limits, token);
        }
    };
}  // namespace

TEST_CASE("Weight painting reaches the final stored layer and preserves exact normalized history", "[terrain][paint]") {
    Fixture fixture;
    const auto before = fixture.document.Source().weights;
    auto operation = fixture.Produce();
    REQUIRE(operation.HasValue());
    REQUIRE(fixture.document.Source().weights == before);
    REQUIRE(operation.Value().patches[0].heightsMeters.empty());
    REQUIRE(operation.Value().patches[0].holes.empty());
    const auto changed = fixture.document.Execute(operation.Value());
    REQUIRE(changed.HasValue());
    REQUIRE(changed.Value().invalidation.visual);
    REQUIRE(changed.Value().invalidation.foliage);
    REQUIRE_FALSE(changed.Value().invalidation.collision);
    REQUIRE_FALSE(changed.Value().invalidation.navigation);
    REQUIRE(changed.Value().dirtyTiles.size() == 4);
    REQUIRE(fixture.document.Source().weights[40 * 16 + 15] == 16384);
    REQUIRE(fixture.document.Source().weights[40 * 16] == 49151);
    for (std::size_t i = 0; i < 81; ++i) {
        auto pixel = std::span(fixture.document.Source().weights).subspan(i * 16, 16);
        REQUIRE(std::accumulate(pixel.begin(), pixel.end(), std::uint32_t{0}) == 65535);
    }
    const auto after = fixture.document.Source().weights;
    REQUIRE(fixture.document.Undo(fixture.document.Fence()).HasValue());
    REQUIRE(fixture.document.Source().weights == before);
    REQUIRE(fixture.document.Redo(fixture.document.Fence()).HasValue());
    REQUIRE(fixture.document.Source().weights == after);
    REQUIRE(fixture.document.Source().layerCount == 16);
}

TEST_CASE("Four active paint targets retain every stored channel and deterministic proportions", "[terrain][paint]") {
    Fixture fixture;
    fixture.brush.targetCount = 4;
    fixture.brush.strength = 1;
    constexpr std::array<std::size_t, 4> indices{3, 7, 11, 15};
    for (std::size_t i = 0; i < 4; ++i)
        fixture.brush.targets[i] = {fixture.binding.order[indices[i]], 1};
    const auto first = fixture.Produce();
    const auto second = fixture.Produce();
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    REQUIRE(first.Value().patches[0].weights == second.Value().patches[0].weights);
    REQUIRE(fixture.document.Execute(first.Value()).HasValue());
    REQUIRE(fixture.document.Source().weights[40 * 16] == 0);
    REQUIRE(fixture.document.Source().weights[40 * 16 + 3] == 16384);
    REQUIRE(fixture.document.Source().weights[40 * 16 + 15] == 16383);
    REQUIRE(fixture.document.Source().weights.size() == 81 * 16);
}

TEST_CASE("Hole carve and restore use the same dirty closure and exact shared history", "[terrain][paint]") {
    Fixture fixture;
    fixture.brush.mode = TerrainPaintMode::CarveHole;
    auto carved = fixture.Produce();
    REQUIRE(carved.HasValue());
    auto change = fixture.document.Execute(carved.Value());
    REQUIRE(change.HasValue());
    REQUIRE(change.Value().invalidation.visual);
    REQUIRE(change.Value().invalidation.collision);
    REQUIRE(change.Value().invalidation.navigation);
    REQUIRE(change.Value().invalidation.foliage);
    REQUIRE(change.Value().dirtyTiles.size() == 4);
    REQUIRE(fixture.document.Source().holes[40] == 1);
    REQUIRE(fixture.document.Source().holes.front() == 0);
    fixture.brush.mode = TerrainPaintMode::RestoreHole;
    auto restored = fixture.Produce();
    REQUIRE(restored.HasValue());
    REQUIRE(fixture.document.Execute(restored.Value()).HasValue());
    REQUIRE(fixture.document.Source().holes[40] == 0);
    REQUIRE(fixture.document.UndoCount() == 2);
    REQUIRE(fixture.document.Undo(fixture.document.Fence()).HasValue());
    REQUIRE(fixture.document.Source().holes[40] == 1);
    REQUIRE(fixture.document.Undo(fixture.document.Fence()).HasValue());
    REQUIRE(fixture.document.Source().holes[40] == 0);
}

TEST_CASE("Paint admission rejects stale semantics cancellation and bounded work before source mutation", "[terrain][paint]") {
    Fixture fixture;
    auto fence = fixture.Fence();
    TerrainSculptLimits limits;
    CancellationSource cancellation;
    SECTION("Semantic revision") {
        fence.layerRevision = Terrain::TerrainContentRevision::Create(4).Value();
    }
    SECTION("Semantic content") {
        fence.layerDigest.bytes[0] ^= 1;
    }
    SECTION("Source revision") {
        fence.document.expectedRevision = Terrain::TerrainSourceRevision::Create(2).Value();
    }
    SECTION("Foreign target") {
        fixture.brush.targets[0].layer = Id<Terrain::TerrainLayerId>(99);
    }
    SECTION("Duplicate target") {
        fixture.brush.targetCount = 2;
        fixture.brush.targets[1] = fixture.brush.targets[0];
    }
    SECTION("Work") {
        limits.maximumSampleVisits = 1;
    }
    SECTION("Patch") {
        limits.maximumPatchSamples = 1;
    }
    SECTION("Cancelled") {
        cancellation.RequestCancellation();
    }
    const auto result = ProduceTerrainPaint(fixture.document.Source(), fixture.binding, fence, TerrainEditOperationId::Create(1).Value(),
                                            fixture.brush, fixture.stamps, limits, cancellation.Token());
    REQUIRE(result.HasError());
    REQUIRE_FALSE(fixture.document.IsDirty());
    REQUIRE(fixture.document.UndoCount() == 0);
    REQUIRE(fixture.document.Fence().expectedRevision.Value() == 1);
}
