#include "Horo/Editor/TerrainBrushTools.h"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <utility>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    TerrainAuthoringDocument BrushDocument() {
        Terrain::TerrainCanonicalSource source;
        Terrain::SerializedTerrainIdentity bytes{};
        bytes.back() = 1;
        source.dataset = Terrain::TerrainDatasetId::Create(bytes).Value();
        source.sourceAsset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value();
        source.revision = Terrain::TerrainSourceRevision::Create(1).Value();
        source.capability = Terrain::TerrainCapabilityRevision::Create(1).Value();
        source.width = source.height = 9;
        source.heightsMeters.assign(81, 0);
        source.holes.assign(81, 0);
        source.layerCount = 3;
        source.weights.resize(243);
        for (std::size_t i = 0; i < 81; ++i) source.weights[i * 3] = 65535;
        return std::move(TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(1).Value(), std::move(source),
            4, TerrainAuthoringCapability::Edit)).Value();
    }
}

TEST_CASE("Terrain brush gesture plans bounded patches and commits exactly once", "[terrain][brush]") {
    auto document = BrushDocument();
    const std::array dabs{TerrainBrushDab{4, 4}, TerrainBrushDab{4, 4}};
    const auto operation = PlanTerrainBrush(document, {.radiusMeters = 1, .strength = 2}, dabs,
        TerrainEditOperationId::Create(1).Value());
    REQUIRE(operation.HasValue());
    REQUIRE(document.UndoCount() == 0);
    REQUIRE(document.Source().heightsMeters[40] == 0);
    REQUIRE(operation.Value().patches.size() == 1);
    REQUIRE(operation.Value().patches[0].heightsMeters.size() <= 9);
    REQUIRE(document.Execute(operation.Value()).HasValue());
    REQUIRE(document.Source().heightsMeters[40] == 4);
    REQUIRE(document.UndoCount() == 1);
    REQUIRE(document.LastUndo()->dirtyTiles.size() == 4);
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE(document.Source().heightsMeters[40] == 0);
    REQUIRE(document.Redo(document.Fence()).HasValue());
    REQUIRE(document.Source().heightsMeters[40] == 4);
}
