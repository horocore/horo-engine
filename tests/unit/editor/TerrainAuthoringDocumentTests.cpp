#include "Horo/Editor/TerrainAuthoringDocument.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    template <typename T> T Id(std::uint8_t value) {
        Terrain::SerializedTerrainIdentity bytes{};
        bytes.back() = value;
        return T::Create(bytes).Value();
    }

    Terrain::TerrainCanonicalSource Source(std::uint32_t width = 9, std::uint32_t height = 9) {
        Terrain::TerrainCanonicalSource source;
        source.dataset = Id<Terrain::TerrainDatasetId>(1);
        source.sourceAsset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value();
        source.revision = Terrain::TerrainSourceRevision::Create(1).Value();
        source.capability = Terrain::TerrainCapabilityRevision::Create(1).Value();
        source.width = width;
        source.height = height;
        source.heightsMeters.assign(width * height, 0.0F);
        source.holes.assign(width * height, 0);
        source.layerCount = 2;
        source.weights.resize(width * height * 2);
        for (std::size_t i = 0; i < source.weights.size(); i += 2)
            source.weights[i] = 65'535;
        return source;
    }

    TerrainAuthoringDocument Document(TerrainEditLimits limits = {}, std::uint64_t session = 1,
                                      TerrainAuthoringCapability capability = TerrainAuthoringCapability::Edit) {
        auto result = TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(session).Value(), Source(), 4, capability, limits);
        return std::move(result).Value();
    }

    TerrainEditOperation Edit(const TerrainAuthoringDocument &document, float height = 7.0F) {
        TerrainEditOperation operation;
        operation.fence = document.Fence();
        operation.operation = TerrainEditOperationId::Create(document.Fence().expectedRevision.Value()).Value();
        operation.patches.push_back({{4, 4, 1, 1}, {height}, {12'345, 53'190}, {1}});
        return operation;
    }

    TerrainAuthoredPlacement Placement(std::uint8_t id = 1, std::uint32_t x = 4) {
        return {Id<Terrain::FoliageInstanceId>(id), Id<Terrain::FoliageTypeId>(1), x, 4, 0, 1, 0};
    }

    template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == error.code.Value());
    }
}  // namespace

TEST_CASE("Terrain owner captures exact patches and shared seam closure", "[terrain][editor]") {
    auto document = Document();
    const auto operation = Edit(document);
    const auto changed = document.Execute(operation);
    REQUIRE(changed.HasValue());
    REQUIRE(changed.Value().changed);
    REQUIRE(changed.Value().dirty);
    REQUIRE(changed.Value().dirtyTiles.size() == 4);
    REQUIRE(changed.Value().invalidation.visual);
    REQUIRE(changed.Value().invalidation.collision);
    REQUIRE(changed.Value().invalidation.navigation);
    REQUIRE(changed.Value().invalidation.foliage);
    REQUIRE(document.Source().heightsMeters[40] == 7);
    REQUIRE(document.Source().holes[40] == 1);
    REQUIRE(document.Source().weights[80] == 12'345);
    REQUIRE(document.Source().heightsMeters[39] == 0);
    REQUIRE(document.LastUndo()->before.size() == 1);
    REQUIRE(document.LastUndo()->before[0].heightsMeters.size() == 1);
    REQUIRE(document.LastUndo()->before[0].heightsMeters[0] == 0);
    REQUIRE(document.LastUndo()->after[0].heightsMeters[0] == 7);
    REQUIRE(document.LastUndo()->before[0].weights[0] == 65'535);
    REQUIRE(document.Fence().expectedRevision.Value() == 2);
    const auto afterState = document.State();
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE(document.Source().heightsMeters[40] == 0);
    REQUIRE(document.Source().holes[40] == 0);
    REQUIRE(document.Source().weights[80] == 65'535);
    REQUIRE_FALSE(document.IsDirty());
    REQUIRE(document.Fence().expectedRevision.Value() == 3);
    REQUIRE(document.Redo(document.Fence()).HasValue());
    REQUIRE(document.State() == afterState);
    REQUIRE(document.Source().heightsMeters[40] == 7);
    REQUIRE(document.Fence().expectedRevision.Value() == 4);
}

TEST_CASE("Terrain failure preserves source revision dirty and both histories", "[terrain][editor]") {
    auto document = Document();
    REQUIRE(document.Execute(Edit(document)).HasValue());
    REQUIRE(document.Undo(document.Fence()).HasValue());
    const auto fence = document.Fence();
    const auto bytes = document.HistoryBytes();
    auto bad = Edit(document);
    SECTION("later patch invalid after earlier valid patch") {
        bad.patches.push_back({{8, 8, 2, 1}, {1, 2}, {}, {}});
    }
    SECTION("channel overlap") {
        bad.patches.push_back({{4, 4, 1, 1}, {3}, {}, {}});
    }
    SECTION("bad normalization") {
        bad.patches[0].weights = {10, 10};
    }
    SECTION("nonfinite sample") {
        bad.patches[0].heightsMeters[0] = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("hole outside binary domain") {
        bad.patches[0].holes[0] = 2;
    }
    SECTION("missing exact samples") {
        bad.patches[0].heightsMeters.push_back(1);
    }
    SECTION("invalid identity") {
        bad.operation = {};
    }
    SECTION("empty operation") {
        bad.patches.clear();
    }
    ErrorIs(document.Execute(bad), TerrainEditErrors::Invalid);
    REQUIRE(document.Source().heightsMeters[40] == 0);
    REQUIRE(document.Fence().expectedRevision == fence.expectedRevision);
    REQUIRE_FALSE(document.IsDirty());
    REQUIRE(document.UndoCount() == 0);
    REQUIRE(document.RedoCount() == 1);
    REQUIRE(document.HistoryBytes() == bytes);
}

TEST_CASE("Terrain no-op and cross-channel overlap do not double-write source", "[terrain][editor]") {
    auto document = Document();
    auto noOp = Edit(document, 0);
    noOp.patches[0].weights = {65'535, 0};
    noOp.patches[0].holes = {0};
    const auto result = document.Execute(noOp);
    REQUIRE(result.HasValue());
    REQUIRE_FALSE(result.Value().changed);
    REQUIRE(document.Fence().expectedRevision.Value() == 1);
    REQUIRE(document.UndoCount() == 0);
    auto operation = Edit(document);
    operation.patches[0].holes.clear();
    operation.patches.push_back({{4, 4, 1, 1}, {}, {}, {1}});
    REQUIRE(document.Execute(operation).HasValue());
    REQUIRE(document.UndoCount() == 1);
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE(document.Source().holes[40] == 0);
}

TEST_CASE("Terrain transactions enforce all finite ceilings before mutation", "[terrain][editor]") {
    TerrainEditLimits limits;
    SECTION("history bytes") {
        limits.maximumHistoryBytes = 1;
    }
    SECTION("transaction bytes") {
        limits.maximumTransactionBytes = 1;
    }
    SECTION("sample visits") {
        limits.maximumSamples = 1;
    }
    SECTION("patch count") {
        limits.maximumPatches = 1;
    }
    SECTION("dirty closure") {
        limits.maximumDirtyTiles = 1;
    }
    auto document = Document(limits);
    auto operation = Edit(document);
    operation.patches.push_back({{1, 1, 1, 1}, {2}, {}, {}});
    ErrorIs(document.Execute(operation), TerrainEditErrors::LimitExceeded);
    REQUIRE(document.Source().heightsMeters[40] == 0);
    REQUIRE(document.Fence().expectedRevision.Value() == 1);
    REQUIRE(document.HistoryBytes() == 0);
}

TEST_CASE("Terrain fences reject stale session revision and capabilities", "[terrain][editor]") {
    auto document = Document();
    auto stale = Edit(document);
    REQUIRE(document.Execute(stale).HasValue());
    ErrorIs(document.Execute(stale), TerrainEditErrors::StaleRevision);
    auto operation = Edit(document);
    SECTION("wrong session") {
        operation.fence.session = TerrainDocumentSessionId::Create(2).Value();
        ErrorIs(document.Execute(operation), TerrainEditErrors::WrongDocument);
    }
    SECTION("wrong dataset") {
        operation.fence.dataset = Id<Terrain::TerrainDatasetId>(2);
        ErrorIs(document.Execute(operation), TerrainEditErrors::WrongDocument);
    }
    SECTION("wrong capability publication") {
        operation.fence.capabilityRevision = Terrain::TerrainCapabilityRevision::Create(2).Value();
        ErrorIs(document.Execute(operation), TerrainEditErrors::CapabilityUnavailable);
    }
    REQUIRE(document.Fence().expectedRevision.Value() == 2);
    REQUIRE(document.UndoCount() == 1);
}

TEST_CASE("Terrain cancellation and close never publish source or history", "[terrain][editor][lifecycle]") {
    auto document = Document();
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    ErrorIs(document.Execute(Edit(document), cancellation.Token()), TerrainEditErrors::Cancelled);
    REQUIRE(document.Execute(Edit(document)).HasValue());
    ErrorIs(document.Undo(document.Fence(), cancellation.Token()), TerrainEditErrors::Cancelled);
    REQUIRE(document.Undo(document.Fence()).HasValue());
    ErrorIs(document.Redo(document.Fence(), cancellation.Token()), TerrainEditErrors::Cancelled);
    const auto pending = Edit(document);
    document.Close();
    document.Close();
    ErrorIs(document.Execute(pending), TerrainEditErrors::Closed);
    ErrorIs(document.Undo(document.Fence()), TerrainEditErrors::Closed);
    ErrorIs(document.Redo(document.Fence()), TerrainEditErrors::Closed);
    ErrorIs(document.AcceptSavedState(document.Fence().session, document.State()), TerrainEditErrors::Closed);
    REQUIRE(document.Source().heightsMeters[40] == 0);
    REQUIRE(document.RedoCount() == 1);
    auto replacement = Document({}, 2);
    ErrorIs(replacement.Execute(pending), TerrainEditErrors::WrongDocument);
    REQUIRE(replacement.Fence().expectedRevision.Value() == 1);
}

TEST_CASE("Terrain dirty tracking survives eviction late save and divergent history", "[terrain][editor]") {
    TerrainEditLimits limits;
    limits.maximumHistoryItems = 1;
    auto document = Document(limits);
    REQUIRE(document.Execute(Edit(document, 4)).HasValue());
    const auto saved = document.State();
    REQUIRE(document.Execute(Edit(document, 8)).HasValue());
    REQUIRE(document.UndoCount() == 1);
    REQUIRE(document.IsDirty());
    REQUIRE(document.AcceptSavedState(document.Fence().session, saved).HasValue());
    REQUIRE(document.IsDirty());
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE_FALSE(document.IsDirty());
    REQUIRE(document.Execute(Edit(document, 9)).HasValue());
    REQUIRE(document.RedoCount() == 0);
    REQUIRE(document.IsDirty());
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE_FALSE(document.IsDirty());
    ErrorIs(document.Undo(document.Fence()), TerrainEditErrors::HistoryEmpty);
    ErrorIs(document.AcceptSavedState(TerrainDocumentSessionId::Create(3).Value(), saved), TerrainEditErrors::WrongDocument);
    ErrorIs(document.AcceptSavedState(document.Fence().session, TerrainDocumentStateId::Create(99).Value()), TerrainEditErrors::Invalid);
}

TEST_CASE("Terrain placement transactions restore exact stable deltas", "[terrain][editor]") {
    auto document = Document();
    auto operation = Edit(document);
    const auto placement = Placement();
    operation.placements.push_back({placement.id, placement});
    REQUIRE(document.Execute(operation).HasValue());
    REQUIRE(document.Placements().at(placement.id) == placement);
    REQUIRE(document.LastUndo()->placements.size() == 1);
    REQUIRE_FALSE(document.LastUndo()->placements[0].before.has_value());
    auto moved = placement;
    moved.sampleX = 8;
    moved.yawRadians = 1;
    TerrainEditOperation move{document.Fence(), TerrainEditOperationId::Create(12).Value(), {}, {{moved.id, moved}}};
    REQUIRE(document.Execute(move).HasValue());
    REQUIRE(document.Placements().at(moved.id) == moved);
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE(document.Placements().at(moved.id) == placement);
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE(document.Placements().empty());
    REQUIRE(document.Redo(document.Fence()).HasValue());
    REQUIRE(document.Redo(document.Fence()).HasValue());
    REQUIRE(document.Placements().at(moved.id) == moved);
    TerrainEditOperation erase{document.Fence(), TerrainEditOperationId::Create(13).Value(), {}, {{moved.id, std::nullopt}}};
    REQUIRE(document.Execute(erase).HasValue());
    REQUIRE(document.Placements().empty());
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE(document.Placements().at(moved.id) == moved);
}

TEST_CASE("Terrain invalid placement does not leave a successful raster prefix", "[terrain][editor]") {
    auto document = Document();
    auto operation = Edit(document);
    auto placement = Placement();
    SECTION("invalid membership") {
        placement.sampleX = 9;
    }
    SECTION("invalid type") {
        placement.type = {};
    }
    SECTION("invalid transform") {
        placement.scale = 0;
    }
    SECTION("different payload identity") {
        placement.id = Id<Terrain::FoliageInstanceId>(2);
    }
    operation.placements.push_back({Id<Terrain::FoliageInstanceId>(1), placement});
    ErrorIs(document.Execute(operation), TerrainEditErrors::Invalid);
    REQUIRE(document.Source().heightsMeters[40] == 0);
    REQUIRE(document.UndoCount() == 0);
    REQUIRE(document.Placements().empty());
}

TEST_CASE("Terrain placement capacity and duplicate IDs fail atomically", "[terrain][editor]") {
    TerrainEditLimits limits;
    limits.maximumPlacements = 1;
    auto document = Document(limits);
    auto operation = Edit(document);
    const auto first = Placement(1), second = Placement(2);
    SECTION("placement capacity") {
        operation.placements = {{first.id, first}, {second.id, second}};
        ErrorIs(document.Execute(operation), TerrainEditErrors::LimitExceeded);
    }
    SECTION("duplicate ID") {
        operation.placements = {{first.id, first}, {first.id, first}};
        ErrorIs(document.Execute(operation), TerrainEditErrors::Invalid);
    }
    SECTION("missing removal") {
        operation.placements = {{first.id, std::nullopt}};
        ErrorIs(document.Execute(operation), TerrainEditErrors::Invalid);
    }
    REQUIRE(document.Placements().empty());
    REQUIRE(document.UndoCount() == 0);
    REQUIRE(document.Source().heightsMeters[40] == 0);
}

TEST_CASE("Terrain source admission read-only and terminal revision boundary", "[terrain][editor]") {
    SECTION("read only") {
        auto document = Document({}, 1, TerrainAuthoringCapability::ReadOnly);
        ErrorIs(document.Execute(Edit(document)), TerrainEditErrors::CapabilityUnavailable);
        ErrorIs(document.Undo(document.Fence()), TerrainEditErrors::CapabilityUnavailable);
        ErrorIs(document.Redo(document.Fence()), TerrainEditErrors::CapabilityUnavailable);
    }
    SECTION("malformed source") {
        auto source = Source();
        source.holes[0] = 2;
        ErrorIs(TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(1).Value(), std::move(source), 4,
                                               TerrainAuthoringCapability::Edit),
                TerrainEditErrors::Invalid);
    }
    SECTION("exhausted revision") {
        auto source = Source();
        source.revision = Terrain::TerrainSourceRevision::Create(std::numeric_limits<std::uint64_t>::max()).Value();
        auto result = TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(1).Value(), std::move(source), 4,
                                                     TerrainAuthoringCapability::Edit);
        REQUIRE(result.HasValue());
        auto document = std::move(result).Value();
        ErrorIs(document.Execute(Edit(document)), TerrainEditErrors::Exhausted);
        REQUIRE(document.Source().heightsMeters[40] == 0);
        REQUIRE(document.HistoryBytes() == 0);
    }
}

TEST_CASE("Terrain dirty tile closure clips outer boundary and includes neighbouring aprons", "[terrain][editor]") {
    auto document = Document();
    auto operation = Edit(document);
    operation.patches = {{{8, 8, 1, 1}, {3}, {}, {}}};
    const auto edge = document.Execute(operation);
    REQUIRE(edge.HasValue());
    REQUIRE(edge.Value().dirtyTiles.size() == 1);
    REQUIRE(edge.Value().dirtyTiles[0].tile.x == 1);
    REQUIRE(edge.Value().dirtyTiles[0].tile.z == 1);
    operation.fence = document.Fence();
    operation.patches = {{{3, 3, 1, 1}, {3}, {}, {}}};
    const auto apron = document.Execute(operation);
    REQUIRE(apron.HasValue());
    REQUIRE(apron.Value().dirtyTiles.size() == 4);
}

TEST_CASE("Terrain opens existing placement source without edit history", "[terrain][editor]") {
    const auto placement = Placement();
    auto result = TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(1).Value(), Source(), 4, TerrainAuthoringCapability::Edit,
                                                 {}, {placement});
    REQUIRE(result.HasValue());
    auto document = std::move(result).Value();
    REQUIRE(document.Placements().at(placement.id) == placement);
    REQUIRE_FALSE(document.IsDirty());
    REQUIRE(document.UndoCount() == 0);
    const TerrainEditOperation remove{document.Fence(), TerrainEditOperationId::Create(1).Value(), {}, {{placement.id, std::nullopt}}};
    REQUIRE(document.Execute(remove).HasValue());
    REQUIRE(document.Placements().empty());
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE(document.Placements().at(placement.id) == placement);
    REQUIRE_FALSE(document.IsDirty());
    ErrorIs(TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(2).Value(), Source(), 4, TerrainAuthoringCapability::Edit, {},
                                           {placement, placement}),
            TerrainEditErrors::Invalid);
}

TEST_CASE("Terrain world-addressed dirty tiles preserve negative origins and reject overflow", "[terrain][editor]") {
    auto source = Source();
    source.coordinates.originX = -1;
    source.coordinates.originZ = -4;
    auto result =
        TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(1).Value(), std::move(source), 4, TerrainAuthoringCapability::Edit);
    REQUIRE(result.HasValue());
    auto document = std::move(result).Value();
    const auto changed = document.Execute(Edit(document));
    REQUIRE(changed.HasValue());
    REQUIRE(changed.Value().dirtyTiles.size() == 4);
    REQUIRE(changed.Value().dirtyTiles.front().tile.x == -1);
    REQUIRE(changed.Value().dirtyTiles.front().tile.z == -1);
    REQUIRE(changed.Value().dirtyTiles.back().tile.x == 0);
    REQUIRE(changed.Value().dirtyTiles.back().tile.z == 0);
    auto invalid = Source();
    invalid.coordinates.originX = static_cast<double>(std::numeric_limits<std::int32_t>::max()) * 4;
    ErrorIs(TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(2).Value(), std::move(invalid), 4,
                                           TerrainAuthoringCapability::Edit),
            TerrainEditErrors::Invalid);
}

TEST_CASE("Terrain snapshots are canonically ordered independent of adapter payload order", "[terrain][editor]") {
    auto document = Document();
    auto operation = Edit(document);
    operation.patches.push_back({{1, 1, 1, 1}, {2}, {}, {}});
    const auto first = Placement(1), second = Placement(2);
    operation.placements = {{second.id, second}, {first.id, first}};
    REQUIRE(document.Execute(operation).HasValue());
    REQUIRE(document.LastUndo()->after.front().rect.x == 1);
    REQUIRE(document.LastUndo()->before.front().rect == document.LastUndo()->after.front().rect);
    REQUIRE(document.LastUndo()->placements.front().id == first.id);
}
