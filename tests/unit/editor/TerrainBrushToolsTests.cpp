#include "Horo/Editor/TerrainBrushTools.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
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
        for (std::size_t i = 0; i < 81; ++i)
            source.weights[i * 3] = 65535;
        return std::move(TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(1).Value(), std::move(source), 4,
                                                        TerrainAuthoringCapability::Edit))
            .Value();
    }
}  // namespace

TEST_CASE("Terrain brush gesture plans bounded patches and commits exactly once", "[terrain][brush]") {
    auto document = BrushDocument();
    const std::array dabs{TerrainBrushDab{4, 4}, TerrainBrushDab{4, 4}};
    const auto operation = PlanTerrainBrush(document, {.radiusMeters = 1, .strength = 2}, dabs, TerrainEditOperationId::Create(1).Value());
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

#include <cstddef>
#include <map>

namespace {
    /** @brief Owns the Lower history fixture and all snapshots through synchronous replay. */
    class LowerHistoryFixture final {
        TerrainAuthoringDocument document;
        const Terrain::TerrainCanonicalSource originalSource;
        const std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> originalPlacements;
        const TerrainEditOperation seed;
        const Result<TerrainEditChange> seeded;
        const TerrainDocumentStateId beforeState;
        const TerrainEditOperation future;
        const Result<TerrainEditChange> futureChanged;
        const Terrain::TerrainCanonicalSource futureSource;
        const Result<TerrainEditChange> restored;
        const Terrain::TerrainCanonicalSource before;
        const TerrainEditFence fence;
        const std::size_t historyBytes;
        const std::array<TerrainBrushDab, 2> dabs;
        const TerrainBrushSettings settings;

    public:
        /** @brief Seeds both history branches without asserting before Lower admission. */
        LowerHistoryFixture()
            : document(BrushDocument()), originalSource(document.Source()), originalPlacements(document.Placements()),
              seed{document.Fence(),
                   TerrainEditOperationId::Create(10).Value(),
                   {{{0, 0, 1, 1}, {7.0F}, {}, {1}}, {{4, 4, 1, 1}, {}, {10'000, 20'000, 35'535}, {}}},
                   {}},
              seeded(document.Execute(seed)), beforeState(document.State()),
              future{document.Fence(), TerrainEditOperationId::Create(11).Value(), {{{4, 4, 1, 1}, {9.0F}, {}, {}}}, {}},
              futureChanged(document.Execute(future)), futureSource(document.Source()), restored(document.Undo(document.Fence())),
              before(document.Source()), fence(document.Fence()), historyBytes(document.HistoryBytes()),
              dabs{TerrainBrushDab{4, 4}, TerrainBrushDab{4, 4}},
              settings{.mode = TerrainBrushMode::Lower, .radiusMeters = 1.0, .strength = 2.0F} {}

        /** @brief Keeps all planner diagnostics alive through both synchronous history probes. */
        void Run() {
            const auto operation = PlanTerrainBrush(document, settings, dabs, TerrainEditOperationId::Create(12).Value());
            INFO("History fixture admission: " << seeded.HasValue() << ", " << futureChanged.HasValue() << ", " << restored.HasValue());
            INFO("Lower planner error domain: " << (operation.HasError() ? operation.ErrorValue().domain.Value() : "none"));
            INFO("Lower planner error code: " << (operation.HasError() ? operation.ErrorValue().code.Value() : "none"));
            REQUIRE(operation.HasValue());
            REQUIRE(seeded.HasValue());
            REQUIRE(futureChanged.HasValue());
            REQUIRE(restored.HasValue());

            const auto revision = fence.expectedRevision.Value();
            requireSource(before, revision);
            REQUIRE(document.State() == beforeState);
            REQUIRE(document.IsDirty());
            REQUIRE(document.UndoCount() == 1);
            REQUIRE(document.RedoCount() == 1);
            REQUIRE(document.HistoryBytes() == historyBytes);
            REQUIRE(document.Fence().session == fence.session);
            REQUIRE(document.Fence().dataset == fence.dataset);
            REQUIRE(document.Fence().capabilityRevision == fence.capabilityRevision);
            REQUIRE(operation.Value().fence.session == fence.session);
            REQUIRE(operation.Value().fence.dataset == fence.dataset);
            REQUIRE(operation.Value().fence.expectedRevision == fence.expectedRevision);
            REQUIRE(operation.Value().fence.capabilityRevision == fence.capabilityRevision);

            ProbeExistingHistory(revision);
            ReplayLowerTransaction(revision);
        }

    private:
        /** @brief Compares canonical source identity and shape without borrowing document state. */
        static void RequireSourceMetadata(const Terrain::TerrainCanonicalSource &actual, const Terrain::TerrainCanonicalSource &expected) {
            REQUIRE((actual.dataset == expected.dataset && actual.sourceAsset == expected.sourceAsset &&
                     actual.capability == expected.capability && actual.width == expected.width && actual.height == expected.height &&
                     actual.layerCount == expected.layerCount));
        }

        /** @brief Compares complete canonical content with an explicit monotonic publication revision. */
        void requireSource(const Terrain::TerrainCanonicalSource &expected, auto revision) const {
            const auto &actual = document.Source();
            RequireSourceMetadata(actual, expected);
            REQUIRE(actual.revision.Value() == revision);
            const auto &a = actual.coordinates;
            const auto &e = expected.coordinates;
            REQUIRE((a.space == e.space && a.projectedCrs == e.projectedCrs && a.originX == e.originX && a.originZ == e.originZ &&
                     a.spacingX == e.spacingX && a.spacingZ == e.spacingZ && a.heightScale == e.heightScale &&
                     a.heightOffset == e.heightOffset && a.maximumPrecisionError == e.maximumPrecisionError));
            REQUIRE(actual.heightsMeters == expected.heightsMeters);
            REQUIRE(actual.weights == expected.weights);
            REQUIRE(actual.holes == expected.holes);
            REQUIRE(document.Placements() == originalPlacements);
        }

        /** @brief Replays both occupied history branches after the nonmutating first plan. */
        void ProbeExistingHistory(auto revision) {
            // Exercise both pre-existing branches, not only their counts, after planning.
            REQUIRE(document.Redo(document.Fence()).HasValue());
            requireSource(futureSource, revision + 1);
            REQUIRE(document.Undo(document.Fence()).HasValue());
            requireSource(before, revision + 2);
            REQUIRE(document.Undo(document.Fence()).HasValue());
            requireSource(originalSource, revision + 3);
            REQUIRE(document.Redo(document.Fence()).HasValue());
            requireSource(before, revision + 4);
            REQUIRE(document.State() == beforeState);
            REQUIRE(document.UndoCount() == 1);
            REQUIRE(document.RedoCount() == 1);
            REQUIRE(document.HistoryBytes() == historyBytes);
        }

        /** @brief Replans against the fresh fence, commits once, and replays exact Lower content. */
        void ReplayLowerTransaction(auto revision) {
            // History replay invalidates the first plan's fence; recapture through the real planner.
            const auto currentOperation = PlanTerrainBrush(document, settings, dabs, TerrainEditOperationId::Create(12).Value());
            REQUIRE(currentOperation.HasValue());
            requireSource(before, revision + 4);
            REQUIRE(document.State() == beforeState);
            REQUIRE(document.UndoCount() == 1);
            REQUIRE(document.RedoCount() == 1);
            REQUIRE(document.HistoryBytes() == historyBytes);
            const auto executed = document.Execute(currentOperation.Value());
            REQUIRE(executed.HasValue());
            REQUIRE(executed.Value().changed);
            REQUIRE(document.UndoCount() == 2);
            REQUIRE(document.RedoCount() == 0);
            REQUIRE(document.LastUndo() != nullptr);
            REQUIRE(document.LastUndo()->operation == currentOperation.Value().operation);
            REQUIRE(before.heightsMeters[40] == 0.0F);
            REQUIRE(before.heightsMeters[0] == 7.0F);
            REQUIRE(document.Source().heightsMeters[40] == -4.0F);
            REQUIRE(document.Source().heightsMeters[0] == before.heightsMeters[0]);
            auto expected = before;
            expected.heightsMeters[40] = -4.0F;
            requireSource(expected, revision + 5);
            const auto committedState = document.State();
            const auto committedBytes = document.HistoryBytes();
            REQUIRE(document.Undo(document.Fence()).HasValue());
            requireSource(before, revision + 6);
            REQUIRE(document.State() == beforeState);
            REQUIRE(document.UndoCount() == 1);
            REQUIRE(document.RedoCount() == 1);
            REQUIRE(document.HistoryBytes() == committedBytes);
            REQUIRE(document.Redo(document.Fence()).HasValue());
            requireSource(expected, revision + 7);
            REQUIRE(document.State() == committedState);
            REQUIRE(document.UndoCount() == 2);
            REQUIRE(document.RedoCount() == 0);
            REQUIRE(document.HistoryBytes() == committedBytes);
        }
    };
}  // namespace

TEST_CASE("Terrain Lower gesture preserves planning history and replays one exact transaction", "[terrain][brush][lower]") {
    LowerHistoryFixture fixture;
    fixture.Run();
}
