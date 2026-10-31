#include "../runtime/terrain/FoliageAuthoringTestSupport.h"
#include "terrain/FoliagePaintProducer.h"
#include "terrain/FoliageSelectionModel.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        using namespace Terrain::FoliageAuthoringTest;

        TerrainAuthoringDocument Document(std::uint64_t session = 1, double originX = 0, double originZ = 0) {
            Terrain::TerrainCanonicalSource source;
            source.dataset = Id<Terrain::TerrainDatasetId>(1);
            source.sourceAsset = Asset(9);
            source.revision = Rev<Terrain::TerrainSourceRevision>(1);
            source.capability = Rev<Terrain::TerrainCapabilityRevision>(1);
            source.width = 4;
            source.height = 4;
            source.heightsMeters.resize(16);
            source.coordinates.originX = originX;
            source.coordinates.originZ = originZ;
            auto opened = TerrainAuthoringDocument::Open(Rev<TerrainDocumentSessionId>(session), std::move(source), 2,
                                                         TerrainAuthoringCapability::Edit);
            REQUIRE(opened.HasValue());
            return std::move(opened).Value();
        }

        FoliagePaletteItem Item() {
            auto source = Terrain::FoliageAuthoringTest::Source();
            auto definition = Terrain::FoliageTypeDefinition::Create(Type().definition, Configuration(), Capabilities());
            REQUIRE(definition.HasValue());
            return {source.definitions[0], definition.Value(), "Actual tracked type"};
        }

        FoliagePaintBrush Brush() {
            FoliagePaintBrush brush;
            brush.radiusMeters = 1;
            brush.gestureKey[0] = std::byte{1};
            return brush;
        }

        TerrainEditOperation Paint(TerrainAuthoringDocument &document, const FoliagePaletteItem &item) {
            const TerrainSculptStamp stamp{1, 1};
            auto produced =
                ProduceFoliagePaint(document, document.Fence(), Rev<TerrainEditOperationId>(1), &item, Brush(), std::span{&stamp, 1}, 2);
            REQUIRE(produced.HasValue());
            return std::move(produced).Value();
        }
    }  // namespace

    TEST_CASE("Foliage paint uses sole document transactions and stable IDs across history and repeated gestures",
              "[foliage-authoring][tools]") {
        auto document = Document();
        auto item = Item();
        auto operation = Paint(document, item);
        REQUIRE(operation.placements.size() == 5);
        CHECK(document.Placements().empty());
        CHECK_FALSE(document.IsDirty());
        REQUIRE(document.Execute(operation).HasValue());
        const auto saved = document.Placements();
        const auto revision = document.Fence().expectedRevision;
        auto repeated = Paint(document, item);
        REQUIRE(repeated.placements.empty());
        // The host omits submission of empty intent; the canonical document deliberately rejects empty operations.
        CHECK(document.Fence().expectedRevision == revision);
        REQUIRE(document.Undo(document.Fence()).HasValue());
        CHECK(document.Placements().empty());
        REQUIRE(document.Redo(document.Fence()).HasValue());
        CHECK(document.Placements() == saved);
    }

    TEST_CASE("Foliage selection observes IDs without ownership and erase history restores exact placements",
              "[foliage-authoring][tools]") {
        auto document = Document();
        auto item = Item();
        REQUIRE(document.Execute(Paint(document, item)).HasValue());
        const auto original = document.Placements();
        const auto revision = document.Fence().expectedRevision;
        TerrainSculptStamp stamp{1, 1};
        auto brush = Brush();
        auto ids = SelectFoliageInBrush(document, document.Fence(), &item, brush, std::span{&stamp, 1});
        REQUIRE(ids.HasValue());
        FoliageSelectionModel selection;
        REQUIRE(selection.Replace(document, document.Fence(), ids.Value()).HasValue());
        CHECK(selection.Ids().size() == 5);
        CHECK(document.Fence().expectedRevision == revision);
        CHECK(document.Placements() == original);
        brush.mode = FoliagePaintMode::Erase;
        brush.allTypes = true;
        auto erased =
            ProduceFoliagePaint(document, document.Fence(), Rev<TerrainEditOperationId>(2), nullptr, brush, std::span{&stamp, 1}, 2);
        REQUIRE(erased.HasValue());
        REQUIRE(document.Execute(erased.Value()).HasValue());
        selection.Observe(document);
        CHECK(selection.Ids().empty());
        REQUIRE(document.Undo(document.Fence()).HasValue());
        CHECK(document.Placements() == original);
        REQUIRE(selection.Replace(document, document.Fence(), ids.Value()).HasValue());
        auto replacement = Document(2);
        selection.Observe(replacement);
        CHECK(selection.Ids().empty());
    }

    TEST_CASE("Foliage palette replacement preserves explicit selection only for a matching retained type", "[foliage-authoring][tools]") {
        FoliagePaletteModel palette;
        auto item = Item();
        REQUIRE(palette.Replace({item}).HasValue());
        CHECK(palette.Active() == nullptr);
        REQUIRE(palette.Choose(item.source.type).HasValue());
        REQUIRE(palette.Active());
        auto wrong = item;
        wrong.source.revision = Rev<Terrain::FoliageDefinitionRevision>(2);
        CHECK(palette.Replace({wrong}).HasError());
        REQUIRE(palette.Active());
        CHECK(palette.Active()->source == item.source);
        REQUIRE(palette.Replace({}).HasValue());
        CHECK(palette.Active() == nullptr);
        palette.Close();
        CHECK(palette.Replace({item}).HasError());
        CHECK(palette.Choose(item.source.type).HasError());
    }

    TEST_CASE("Foliage tool failures cancel or reject atomically at exact bounds and stale lifecycle", "[foliage-authoring][tools]") {
        auto document = Document();
        auto item = Item();
        const TerrainSculptStamp stamp{1, 1};
        auto brush = Brush();
        FoliageToolLimits limits;
        limits.maximumPlacementEdits = 1;
        CHECK(ProduceFoliagePaint(document, document.Fence(), Rev<TerrainEditOperationId>(1), &item, brush, std::span{&stamp, 1}, 2, limits)
                  .HasError());
        auto stale = document.Fence();
        stale.expectedRevision = Rev<Terrain::TerrainSourceRevision>(2);
        CHECK(ProduceFoliagePaint(document, stale, Rev<TerrainEditOperationId>(1), &item, brush, std::span{&stamp, 1}, 2).HasError());
        brush.scale = std::numeric_limits<float>::quiet_NaN();
        CHECK(ProduceFoliagePaint(document, document.Fence(), Rev<TerrainEditOperationId>(1), &item, brush, std::span{&stamp, 1}, 2)
                  .HasError());
        CancellationSource cancel;
        cancel.RequestCancellation();
        brush = Brush();
        CHECK(ProduceFoliagePaint(document, document.Fence(), Rev<TerrainEditOperationId>(1), &item, brush, std::span{&stamp, 1}, 2, {},
                                  cancel.Token())
                  .HasError());
        CHECK(document.Placements().empty());
        CHECK(document.UndoCount() == 0);
        CHECK_FALSE(document.IsDirty());
        document.Close();
        CHECK(ProduceFoliagePaint(document, document.Fence(), Rev<TerrainEditOperationId>(1), &item, brush, std::span{&stamp, 1}, 2)
                  .HasError());
    }

    TEST_CASE("Foliage paint identity uses actual negative world tiles and rejects coordinate overflow", "[foliage-authoring][tools]") {
        auto document = Document(1, -3, 3);
        auto item = Item();
        auto painted = Paint(document, item);
        const auto found = std::ranges::find_if(painted.placements, [](const auto &edit) {
            return edit.value->sampleX == 1 && edit.value->sampleZ == 1;
        });
        REQUIRE(found != painted.placements.end());
        // Exact authoring-v1 provenance for gesture 01..., sample (1,1), in actual world tile (-1,2).
        std::array<std::byte, 32> key{};
        key[0] = std::byte{'a'};
        key[1] = std::byte{'u'};
        key[2] = std::byte{'t'};
        key[3] = std::byte{'h'};
        key[4] = std::byte{'o'};
        key[5] = std::byte{'r'};
        key[6] = std::byte{1};
        key[8] = std::byte{1};
        key[24] = std::byte{1};
        key[28] = std::byte{1};
        auto expected = Terrain::DeriveFoliageInstanceId({document.Source().dataset, {-1, 2, 0}}, item.source.type, key);
        REQUIRE(expected.HasValue());
        CHECK(found->id == expected.Value());
        auto invalidSource = document.Source();
        invalidSource.coordinates.originX = double(std::numeric_limits<std::int32_t>::max()) * 4;
        auto rejected =
            TerrainAuthoringDocument::Open(Rev<TerrainDocumentSessionId>(2), std::move(invalidSource), 2, TerrainAuthoringCapability::Edit);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == TerrainEditErrors::Invalid.code.Value());
        // This document is valid for its real two-cell layout. A malformed external one-cell tool layout must still reject safely.
        auto overflow = Document(3, double(std::numeric_limits<std::int32_t>::max()) * 2 - 4, 0);
        const TerrainSculptStamp stamp{1, 1};
        CHECK(ProduceFoliagePaint(overflow, overflow.Fence(), Rev<TerrainEditOperationId>(1), &item, Brush(), std::span{&stamp, 1}, 1)
                  .HasError());
        CHECK(overflow.Placements().empty());
        CHECK_FALSE(overflow.IsDirty());
    }
}  // namespace Horo::Editor
