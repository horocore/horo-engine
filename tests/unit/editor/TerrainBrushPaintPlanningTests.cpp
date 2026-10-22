#include "Horo/Editor/TerrainBrushTools.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    /** @brief Admits real stable identities before constructing canonical fixture values. */
    template <typename T> T PaintIdentity(std::uint8_t value) {
        Terrain::SerializedTerrainIdentity bytes{};
        bytes.back() = value;
        const auto identity = T::Create(bytes);
        REQUIRE(identity.HasValue());
        return identity.Value();
    }

    /** @brief Supplies three authored UNORM16 layers with a positive exact sum at every sample. */
    Terrain::TerrainCanonicalSource PaintSource() {
        Terrain::TerrainCanonicalSource source;
        source.dataset = PaintIdentity<Terrain::TerrainDatasetId>(1);
        const auto asset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001");
        const auto revision = Terrain::TerrainSourceRevision::Create(1);
        const auto capability = Terrain::TerrainCapabilityRevision::Create(1);
        REQUIRE(asset.HasValue());
        REQUIRE(revision.HasValue());
        REQUIRE(capability.HasValue());
        source.sourceAsset = asset.Value();
        source.revision = revision.Value();
        source.capability = capability.Value();
        source.width = source.height = 9;
        source.heightsMeters.assign(81, 2.0F);
        source.heightsMeters[0] = 7.0F;
        source.holes.assign(81, 0);
        source.holes[0] = 1;
        source.layerCount = 3;
        for (std::size_t pixel = 0; pixel < 81; ++pixel)
            source.weights.insert(source.weights.end(), {10'000, 20'000, 35'535});
        return source;
    }

    /** @brief Requires canonical document admission, including a nonempty authored placement, before Paint planning. */
    TerrainAuthoringDocument PaintDocument() {
        const auto session = TerrainDocumentSessionId::Create(1);
        REQUIRE(session.HasValue());
        const TerrainAuthoredPlacement
            placement{PaintIdentity<Terrain::FoliageInstanceId>(2), PaintIdentity<Terrain::FoliageTypeId>(3), 4, 4, 0.5F, 1.0F, 0.25F};
        auto opened = TerrainAuthoringDocument::Open(session.Value(), PaintSource(), 4, TerrainAuthoringCapability::Edit, {}, {placement});
        REQUIRE(opened.HasValue());
        return std::move(opened).Value();
    }

    /** @brief Compares all four typed fence domains without substituting content-state identity. */
    void RequirePaintFence(const TerrainEditFence &actual, const TerrainEditFence &expected) {
        REQUIRE(actual.session == expected.session);
        REQUIRE(actual.dataset == expected.dataset);
        REQUIRE(actual.expectedRevision == expected.expectedRevision);
        REQUIRE(actual.capabilityRevision == expected.capabilityRevision);
    }

    /** @brief Keeps the complete source coordinate policy invariant across planning and history replay. */
    void RequirePaintCoordinates(const Terrain::TerrainSourceCoordinates &actual, const Terrain::TerrainSourceCoordinates &expected) {
        REQUIRE((actual.space == expected.space && actual.projectedCrs == expected.projectedCrs && actual.originX == expected.originX &&
                 actual.originZ == expected.originZ && actual.spacingX == expected.spacingX && actual.spacingZ == expected.spacingZ &&
                 actual.heightScale == expected.heightScale && actual.heightOffset == expected.heightOffset &&
                 actual.maximumPrecisionError == expected.maximumPrecisionError));
    }

    /** @brief Requires one center-sample weight payload and no height or hole writes. */
    void RequirePaintPatch(const TerrainRasterPatch &patch, const std::vector<std::uint16_t> &weights) {
        REQUIRE((patch.rect == TerrainPatchRect{4, 4, 1, 1}));
        REQUIRE(patch.heightsMeters.empty());
        REQUIRE(patch.holes.empty());
        REQUIRE(patch.weights == weights);
    }

    /** @brief Owns canonical snapshots through the one planning, commit and semantic replay tracer. */
    class PaintHistoryFixture final {
        TerrainAuthoringDocument document;
        const Terrain::TerrainCanonicalSource original;
        const std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> placements;
        const TerrainEditFence fence;
        const TerrainDocumentStateId initialState;
        const std::size_t initialBytes;

    public:
        /** @brief Takes only an already-admitted real canonical document. */
        explicit PaintHistoryFixture(TerrainAuthoringDocument owner)
            : document(std::move(owner)), original(document.Source()), placements(document.Placements()), fence(document.Fence()),
              initialState(document.State()), initialBytes(document.HistoryBytes()) {}

        /** @brief The sole missing-feature assertion is successful Paint operation admission. */
        void Run() {
            const auto identity = TerrainEditOperationId::Create(12);
            REQUIRE(identity.HasValue());
            const std::array dabs{TerrainBrushDab{4, 4}};
            const TerrainBrushSettings settings{.mode = TerrainBrushMode::Paint, .radiusMeters = 0.5, .strength = 1.0F, .layer = 1};
            const auto operation = PlanTerrainBrush(document, settings, dabs, identity.Value());
            INFO("Paint planner error domain: " << (operation.HasError() ? operation.ErrorValue().domain.Value() : "none"));
            INFO("Paint planner error code: " << (operation.HasError() ? operation.ErrorValue().code.Value() : "none"));
            RequireContent(original, fence.expectedRevision.Value());
            RequireHistory(initialState, 0, 0, false);
            RequirePaintFence(document.Fence(), fence);
            REQUIRE(document.HistoryBytes() == initialBytes);
            REQUIRE(document.LastUndo() == nullptr);
            REQUIRE(operation.HasValue());
            RequirePaintFence(operation.Value().fence, fence);
            REQUIRE(operation.Value().operation == identity.Value());
            REQUIRE(operation.Value().placements.empty());
            REQUIRE(operation.Value().patches.size() == 1);
            RequirePaintPatch(operation.Value().patches[0], {0, 65'535, 0});
            auto painted = original;
            painted.weights[120] = 0;
            painted.weights[121] = 65'535;
            painted.weights[122] = 0;
            CommitAndReplay(operation.Value(), painted);
        }

    private:
        /** @brief Compares complete canonical content, metadata and nonempty placement source at each publication. */
        void RequireContent(const Terrain::TerrainCanonicalSource &expected, std::uint64_t revision) const {
            const auto &actual = document.Source();
            REQUIRE((actual.dataset == expected.dataset && actual.sourceAsset == expected.sourceAsset &&
                     actual.capability == expected.capability && actual.width == expected.width && actual.height == expected.height &&
                     actual.layerCount == expected.layerCount));
            RequirePaintCoordinates(actual.coordinates, expected.coordinates);
            REQUIRE(actual.revision.Value() == revision);
            REQUIRE(actual.heightsMeters == expected.heightsMeters);
            REQUIRE(actual.weights == expected.weights);
            REQUIRE(actual.holes == expected.holes);
            REQUIRE(document.Placements() == placements);
        }

        /** @brief Distinguishes semantic dirty/state restoration from monotonically increasing source revisions. */
        void RequireHistory(TerrainDocumentStateId state, std::size_t undo, std::size_t redo, bool dirty) const {
            REQUIRE(document.State() == state);
            REQUIRE(document.UndoCount() == undo);
            REQUIRE(document.RedoCount() == redo);
            REQUIRE(document.IsDirty() == dirty);
        }

        /** @brief Checks the actual weight-only visual/foliage invalidation and exact four-tile seam/apron closure. */
        void RequireChange(const TerrainEditChange &change, TerrainEditOperationId operation, std::uint64_t revision,
                           TerrainDocumentStateId state, bool dirty) const {
            std::vector<Terrain::TerrainTileId> tiles{{original.dataset, {0, 0, 0}},
                                                      {original.dataset, {0, 1, 0}},
                                                      {original.dataset, {1, 0, 0}},
                                                      {original.dataset, {1, 1, 0}}};
            std::ranges::sort(tiles);
            REQUIRE(change.operation == operation);
            REQUIRE(change.revision.Value() == revision);
            REQUIRE(change.state == state);
            REQUIRE(change.changed);
            REQUIRE(change.dirty == dirty);
            REQUIRE(change.dirtyTiles == tiles);
            REQUIRE(change.invalidation.visual);
            REQUIRE(change.invalidation.foliage);
            REQUIRE_FALSE(change.invalidation.collision);
            REQUIRE_FALSE(change.invalidation.navigation);
        }

        /** @brief Requires lossless region-only history with the original operation and no collateral channel snapshots. */
        void RequireRecord(const TerrainEditOperation &operation) const {
            const auto *record = document.LastUndo();
            REQUIRE(record != nullptr);
            REQUIRE(record->operation == operation.operation);
            REQUIRE(record->baseRevision == fence.expectedRevision);
            REQUIRE(record->committedRevision == document.Fence().expectedRevision);
            REQUIRE(record->beforeState == initialState);
            REQUIRE(record->afterState == document.State());
            REQUIRE(record->before.size() == 1);
            REQUIRE(record->after.size() == 1);
            REQUIRE(record->placements.empty());
            RequirePaintPatch(record->before[0], {10'000, 20'000, 35'535});
            RequirePaintPatch(record->after[0], {0, 65'535, 0});
            REQUIRE(document.HistoryBytes() == record->bytes);
            REQUIRE(record->bytes > initialBytes);
        }

        /** @brief Commits through Execute, then restores exact normalized values without rerunning the brush. */
        void CommitAndReplay(const TerrainEditOperation &operation, const Terrain::TerrainCanonicalSource &painted) {
            const auto revision = fence.expectedRevision.Value();
            const auto committed = document.Execute(operation);
            REQUIRE(committed.HasValue());
            const auto paintedState = document.State();
            REQUIRE(paintedState != initialState);
            RequireChange(committed.Value(), operation.operation, revision + 1, paintedState, true);
            RequireContent(painted, revision + 1);
            RequireHistory(paintedState, 1, 0, true);
            RequireRecord(operation);
            const auto bytes = document.HistoryBytes();
            const auto undone = document.Undo(document.Fence());
            REQUIRE(undone.HasValue());
            RequireChange(undone.Value(), operation.operation, revision + 2, initialState, false);
            RequireContent(original, revision + 2);
            RequireHistory(initialState, 0, 1, false);
            REQUIRE(document.HistoryBytes() == bytes);
            const auto redone = document.Redo(document.Fence());
            REQUIRE(redone.HasValue());
            RequireChange(redone.Value(), operation.operation, revision + 3, paintedState, true);
            RequireContent(painted, revision + 3);
            RequireHistory(paintedState, 1, 0, true);
            REQUIRE(document.HistoryBytes() == bytes);
        }
    };
}  // namespace

TEST_CASE("Terrain Paint plans one normalized weight patch and replays exact history", "[terrain][brush][paint]") {
    PaintHistoryFixture fixture{PaintDocument()};
    fixture.Run();
}
