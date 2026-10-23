#include "Horo/Editor/TerrainBrushTools.h"

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
    /** @brief Admits stable authored identities without borrowing mutable source storage. */
    template <typename T> T CarveIdentity(std::uint8_t value) {
        Terrain::SerializedTerrainIdentity bytes{};
        bytes.back() = value;
        const auto identity = T::Create(bytes);
        REQUIRE(identity.HasValue());
        return identity.Value();
    }

    /** @brief Admits a real masked source and an exact public hole patch before brush planning. */
    TerrainAuthoringDocument CarveDocument() {
        Terrain::TerrainCanonicalSource source;
        source.dataset = CarveIdentity<Terrain::TerrainDatasetId>(1);
        const auto asset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001");
        const auto revision = Terrain::TerrainSourceRevision::Create(1);
        const auto capability = Terrain::TerrainCapabilityRevision::Create(1);
        const auto session = TerrainDocumentSessionId::Create(1);
        const auto seedId = TerrainEditOperationId::Create(10);
        REQUIRE(asset.HasValue());
        REQUIRE(revision.HasValue());
        REQUIRE(capability.HasValue());
        REQUIRE(session.HasValue());
        REQUIRE(seedId.HasValue());
        source.sourceAsset = asset.Value();
        source.revision = revision.Value();
        source.capability = capability.Value();
        source.width = source.height = 9;
        source.heightsMeters.assign(81, 2.0F);
        source.heightsMeters[0] = 7.0F;
        source.holes.assign(81, 0);
        source.holes[0] = 1;
        source.layerCount = 3;
        for (std::size_t sample = 0; sample < 81; ++sample)
            source.weights.insert(source.weights.end(), {10'000, 20'000, 35'535});
        const TerrainAuthoredPlacement
            placement{CarveIdentity<Terrain::FoliageInstanceId>(2), CarveIdentity<Terrain::FoliageTypeId>(3), 2, 2, 0.5F, 1.0F, 0.25F};
        auto opened =
            TerrainAuthoringDocument::Open(session.Value(), std::move(source), 4, TerrainAuthoringCapability::Edit, {}, {placement});
        REQUIRE(opened.HasValue());
        auto document = std::move(opened).Value();
        const TerrainEditOperation seed{document.Fence(), seedId.Value(), {{{7, 7, 1, 1}, {}, {}, {1}}}, {}};
        REQUIRE(document.Execute(seed).HasValue());
        REQUIRE(document.Source().holes[70] == 1);
        REQUIRE(document.Undo(document.Fence()).HasValue());
        REQUIRE(document.Source().holes[70] == 0);
        REQUIRE(document.RedoCount() == 1);
        return document;
    }

    /** @brief Canonical holes are per-vertex source samples, not independently indexed quad cells. */
    void RequireCarvePatch(const TerrainRasterPatch &patch, std::uint8_t hole) {
        REQUIRE((patch.rect == TerrainPatchRect{2, 2, 1, 1}));
        REQUIRE(patch.heightsMeters.empty());
        REQUIRE(patch.weights.empty());
        REQUIRE(patch.holes == std::vector<std::uint8_t>{hole});
    }

    class CarveHistoryFixture final {
        TerrainAuthoringDocument document;
        const Terrain::TerrainCanonicalSource original;
        const std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> placements;
        const TerrainEditFence fence;
        const TerrainDocumentStateId initialState;
        const std::size_t initialBytes;

    public:
        /** @brief Owns admitted canonical content and the retained redo record throughout this tracer. */
        explicit CarveHistoryFixture(TerrainAuthoringDocument owner)
            : document(std::move(owner)), original(document.Source()), placements(document.Placements()), fence(document.Fence()),
              initialState(document.State()), initialBytes(document.HistoryBytes()) {}

        /** @brief One exact endpoint gesture: no fractional threshold, seam or presentation dependencies. */
        void Run() {
            const auto identity = TerrainEditOperationId::Create(12);
            REQUIRE(identity.HasValue());
            REQUIRE(original.HasHoles());
            REQUIRE(original.holes.size() == 81);
            REQUIRE(original.holes[20] == 0);
            REQUIRE(original.holes[0] == 1);
            REQUIRE(original.heightsMeters[0] == 7.0F);
            REQUIRE(original.weights.size() == 243);
            REQUIRE(placements.size() == 1);
            REQUIRE(initialBytes > 0);
            RequireContent(original, 3);
            RequireHistory(initialState, 0, 1, false);
            REQUIRE(document.LastUndo() == nullptr);
            const std::array dabs{TerrainBrushDab{2, 2}};
            const TerrainBrushSettings settings{.mode = TerrainBrushMode::Carve, .radiusMeters = 0.25, .strength = 1.0F};
            const TerrainBrushLimits limits{.maximumDabs = 1, .maximumSamples = 16, .maximumReadVisits = 64, .maximumOwnedBytes = 4096};
            const auto operation = PlanTerrainBrush(document, settings, dabs, identity.Value(), limits);
            INFO("Carve error domain: " << (operation.HasError() ? operation.ErrorValue().domain.Value() : "none"));
            INFO("Carve error code: " << (operation.HasError() ? operation.ErrorValue().code.Value() : "none"));
            RequireContent(original, 3);
            RequireHistory(initialState, 0, 1, false);
            REQUIRE(document.HistoryBytes() == initialBytes);
            REQUIRE(document.LastUndo() == nullptr);
            REQUIRE(operation.HasValue());
            const auto &planned = operation.Value();
            REQUIRE(planned.fence.session == fence.session);
            REQUIRE(planned.fence.dataset == fence.dataset);
            REQUIRE(planned.fence.expectedRevision == fence.expectedRevision);
            REQUIRE(planned.fence.capabilityRevision == fence.capabilityRevision);
            REQUIRE(planned.operation == identity.Value());
            REQUIRE(planned.placements.empty());
            REQUIRE(planned.patches.size() == 1);
            RequireCarvePatch(planned.patches[0], 1);
            CommitAndReplay(planned);
        }

    private:
        /** @brief Full-array comparisons prove every non-target sample and authored placement remains exact. */
        void RequireContent(const Terrain::TerrainCanonicalSource &expected, std::uint64_t revision) const {
            const auto &actual = document.Source();
            REQUIRE(actual.dataset == expected.dataset);
            REQUIRE(actual.sourceAsset == expected.sourceAsset);
            REQUIRE(actual.capability == expected.capability);
            REQUIRE(actual.width == expected.width);
            REQUIRE(actual.height == expected.height);
            REQUIRE(actual.layerCount == expected.layerCount);
            const auto &a = actual.coordinates;
            const auto &e = expected.coordinates;
            REQUIRE(a.space == e.space);
            REQUIRE(a.projectedCrs == e.projectedCrs);
            REQUIRE(a.originX == e.originX);
            REQUIRE(a.originZ == e.originZ);
            REQUIRE(a.spacingX == e.spacingX);
            REQUIRE(a.spacingZ == e.spacingZ);
            REQUIRE(a.heightScale == e.heightScale);
            REQUIRE(a.heightOffset == e.heightOffset);
            REQUIRE(a.maximumPrecisionError == e.maximumPrecisionError);
            REQUIRE(actual.revision.Value() == revision);
            REQUIRE(actual.heightsMeters == expected.heightsMeters);
            REQUIRE(actual.weights == expected.weights);
            REQUIRE(actual.holes == expected.holes);
            REQUIRE(document.Placements() == placements);
            REQUIRE(document.Fence().session == fence.session);
            REQUIRE(document.Fence().dataset == fence.dataset);
            REQUIRE(document.Fence().capabilityRevision == fence.capabilityRevision);
            REQUIRE(document.Fence().expectedRevision == actual.revision);
        }

        /** @brief Keeps saved semantic state distinct from monotonic revision and occupied redo storage. */
        void RequireHistory(TerrainDocumentStateId state, std::size_t undo, std::size_t redo, bool dirty) const {
            REQUIRE(document.State() == state);
            REQUIRE(document.UndoCount() == undo);
            REQUIRE(document.RedoCount() == redo);
            REQUIRE(document.IsDirty() == dirty);
        }

        /** @brief The interior sample's clipped apron stays in tile zero; holes invalidate every existing dependency. */
        void RequireChange(const TerrainEditChange &change, TerrainEditOperationId operation, std::uint64_t revision,
                           TerrainDocumentStateId state, bool dirty) const {
            const std::vector<Terrain::TerrainTileId> tiles{{original.dataset, {0, 0, 0}}};
            REQUIRE(change.operation == operation);
            REQUIRE(change.revision.Value() == revision);
            REQUIRE(change.state == state);
            REQUIRE(change.changed);
            REQUIRE(change.dirty == dirty);
            REQUIRE(change.dirtyTiles == tiles);
            REQUIRE(change.invalidation.visual);
            REQUIRE(change.invalidation.foliage);
            REQUIRE(change.invalidation.collision);
            REQUIRE(change.invalidation.navigation);
        }

        /** @brief Region-only semantic snapshots retain the host operation identity without copying other channels. */
        void RequireRecord(const TerrainEditOperation &operation) const {
            const auto *record = document.LastUndo();
            REQUIRE(record != nullptr);
            REQUIRE(record->operation == operation.operation);
            REQUIRE(record->baseRevision == fence.expectedRevision);
            REQUIRE(record->committedRevision.Value() == 4);
            REQUIRE(record->beforeState == initialState);
            REQUIRE(record->afterState == document.State());
            REQUIRE(record->before.size() == 1);
            REQUIRE(record->after.size() == 1);
            REQUIRE(record->placements.empty());
            RequireCarvePatch(record->before[0], 0);
            RequireCarvePatch(record->after[0], 1);
            REQUIRE(document.HistoryBytes() == record->bytes);
            REQUIRE(record->bytes > 0);
            REQUIRE((record->dirtyTiles == std::vector<Terrain::TerrainTileId>{{original.dataset, {0, 0, 0}}}));
            REQUIRE((record->invalidation.visual && record->invalidation.foliage && record->invalidation.collision &&
                     record->invalidation.navigation));
        }

        /** @brief Uses a legally opened foreign owner and a naturally stale fence, never manufactured source mutation. */
        void RequireRejections(const TerrainEditOperation &operation, const Terrain::TerrainCanonicalSource &carved) {
            const auto foreignSession = TerrainDocumentSessionId::Create(2);
            REQUIRE(foreignSession.HasValue());
            auto foreign = TerrainAuthoringDocument::Open(foreignSession.Value(), original, 4, TerrainAuthoringCapability::Edit);
            REQUIRE(foreign.HasValue());
            auto foreignDocument = std::move(foreign).Value();
            const auto foreignResult = foreignDocument.Execute(operation);
            REQUIRE(foreignResult.HasError());
            REQUIRE(foreignResult.ErrorValue().code == TerrainEditErrors::WrongDocument.code);
            REQUIRE(foreignDocument.Source().holes == original.holes);
            REQUIRE(foreignDocument.Source().heightsMeters == original.heightsMeters);
            REQUIRE(foreignDocument.Source().weights == original.weights);
            REQUIRE(foreignDocument.UndoCount() == 0);
            REQUIRE(foreignDocument.RedoCount() == 0);
            REQUIRE_FALSE(foreignDocument.IsDirty());
            const auto bytes = document.HistoryBytes();
            const auto state = document.State();
            const auto rejected = document.Execute(operation);
            REQUIRE(rejected.HasError());
            REQUIRE(rejected.ErrorValue().code == TerrainEditErrors::StaleRevision.code);
            const auto staleUndo = document.Undo(operation.fence);
            REQUIRE(staleUndo.HasError());
            REQUIRE(staleUndo.ErrorValue().code == TerrainEditErrors::StaleRevision.code);
            RequireContent(carved, 4);
            RequireHistory(state, 1, 0, true);
            REQUIRE(document.HistoryBytes() == bytes);
            RequireRecord(operation);
        }

        /** @brief Executes once, discards prior redo, then replays exact values with new publication revisions. */
        void CommitAndReplay(const TerrainEditOperation &operation) {
            auto carved = original;
            carved.holes[20] = 1;
            const auto committed = document.Execute(operation);
            REQUIRE(committed.HasValue());
            const auto carvedState = document.State();
            REQUIRE(carvedState != initialState);
            RequireChange(committed.Value(), operation.operation, 4, carvedState, true);
            RequireContent(carved, 4);
            RequireHistory(carvedState, 1, 0, true);
            RequireRecord(operation);
            RequireRejections(operation, carved);
            const auto bytes = document.HistoryBytes();
            const auto undone = document.Undo(document.Fence());
            REQUIRE(undone.HasValue());
            RequireChange(undone.Value(), operation.operation, 5, initialState, false);
            RequireContent(original, 5);
            RequireHistory(initialState, 0, 1, false);
            REQUIRE(document.HistoryBytes() == bytes);
            const auto redone = document.Redo(document.Fence());
            REQUIRE(redone.HasValue());
            RequireChange(redone.Value(), operation.operation, 6, carvedState, true);
            RequireContent(carved, 6);
            RequireHistory(carvedState, 1, 0, true);
            REQUIRE(document.HistoryBytes() == bytes);
            RequireRecord(operation);
        }
    };
}  // namespace

TEST_CASE("Terrain Carve plans one hole sample and replays exact fenced history", "[terrain][brush][carve]") {
    CarveHistoryFixture fixture{CarveDocument()};
    fixture.Run();
}
