#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <stop_token>
#include <string_view>
#include <thread>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        Assets::AssetId InspectionAsset(const std::uint8_t value) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return Assets::AssetId::FromBytes(bytes);
        }

        WS::WorldPartitionBounds Bounds(const std::int64_t minimum, const std::int64_t maximum) {
            return {Math::WorldCoordinate64::FromMillimeters(minimum, 0, 0), Math::WorldCoordinate64::FromMillimeters(maximum, 0, 0)};
        }

        PartitionedWorldDocument InspectionDocument() {
            const auto world = WS::WorldPartitionId::Create(InspectionAsset(1).Bytes()).Value();
            auto grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-2, 2, -2, 2, -2, 2}, 2).Value();
            const auto revision = WS::WorldAuthoringRevision::Create(1).Value();
            PartitionedWorldDocument document{.settings = {.partition = world, .bounds = Bounds(-2000, 2999), .grid = grid}};
            const auto layer = WS::StreamingLayerId::Create(0).Value();
            document.layers.push_back({.id = layer, .name = "Geometry"});
            for (const auto page : {InspectionAsset(2), InspectionAsset(3)}) {
                PartitionedWorldPage source{.head = {.partition = world, .sourceAsset = page, .revision = revision}};
                source.objects.push_back({.descriptor = {.address = {page, 1},
                                                         .revision = revision,
                                                         .sourceAsset = InspectionAsset(4),
                                                         .bounds = Bounds(50, 99)},
                                          .layer = layer});
                document.pages.push_back(std::move(source));
            }
            return document;
        }

        template <typename T> void InspectionError(const Result<T> &result, const ErrorCodeDescriptor &code) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code == code.code);
        }

        PartitionedWorldSnapshotLease SelectFirst(PartitionedWorldWorkspace &workspace, const PartitionedWorldDocument &document) {
            auto opened = workspace.Open(document);
            REQUIRE(opened.HasValue());
            const auto selected = workspace.SetSelection(std::array{document.pages.front().objects.front().descriptor.address},
                                                         document.pages.front().objects.front().descriptor.address, opened.Value()->fence);
            REQUIRE(selected.HasValue());
            return selected.Value();
        }

        WS::StreamingCellId Cell(const std::int32_t x, const std::uint8_t lod = 0) {
            return {x, 0, 0, lod, WS::StreamingLayerId::Create(0).Value()};
        }

        TEST_CASE("World object inspector uses the owning selection and exact immutable source fence",
                  "[unit][editor][world_document][object_inspection]") {
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            const auto document = InspectionDocument();
            const auto opened = workspace.Open(document).Value();
            auto addresses = std::array{document.pages[1].objects[0].descriptor.address, document.pages[0].objects[0].descriptor.address};
            auto selected = workspace.SetSelection(addresses, addresses[0], opened->fence).Value();
            REQUIRE_FALSE(selected->dirty);
            REQUIRE(selected->openPages.empty());
            REQUIRE(opened->selectedObjects.empty());
            InspectionError(workspace.InspectSelectedObjects(opened->fence), WorldDocumentErrors::Stale);
            const auto inspection = workspace.InspectSelectedObjects(selected->fence).Value();
            REQUIRE(inspection->fence == selected->fence);
            REQUIRE(inspection->primary == addresses[0]);
            REQUIRE(inspection->objects.size() == 2);
            REQUIRE(inspection->objects[0].source.descriptor.address == addresses[0]);
            REQUIRE(inspection->objects[1].source.descriptor.address == addresses[1]);
            REQUIRE(inspection->objects[0].placement == WS::WorldSpanningObjectPlacement::Direct);
            REQUIRE(inspection->objects[0].automaticCells == std::vector{Cell(0)});
            REQUIRE(inspection->objects[0].placementCells == std::vector{Cell(0)});
            REQUIRE_FALSE(inspection->objects[0].diagnostic);
            REQUIRE(workspace.SetSelection(addresses, addresses[0], selected->fence).Value() == selected);
            addresses[0] = {};
            REQUIRE(workspace.Snapshot().Value() == selected);
            REQUIRE(inspection->objects[0].source.descriptor.address.IsValid());
        }

        TEST_CASE("World object inspector preserves inclusive bounds and canonical negative and LOD quantization",
                  "[unit][editor][world_document][object_inspection]") {
            auto document = InspectionDocument();
            document.settings.maximumDirectCellsPerObject = 4;
            auto &object = document.pages[0].objects[0];
            std::vector<WS::StreamingCellId> expected;
            SECTION("point exactly on a positive boundary") {
                object.descriptor.bounds = Bounds(1000, 1000);
                expected = {Cell(1)};
            }
            SECTION("inclusive maximum touches the next half-open cell") {
                object.descriptor.bounds = Bounds(-1, 0);
                expected = {Cell(-1), Cell(0)};
            }
            SECTION("negative floor division") {
                object.descriptor.bounds = Bounds(-1001, -1000);
                expected = {Cell(-2), Cell(-1)};
            }
            SECTION("authored LOD uses doubled cell size") {
                object.descriptor.bounds = Bounds(0, 1999);
                object.assignmentLod = 1;
                expected = {Cell(0, 1)};
            }
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            const auto selected = SelectFirst(workspace, document);
            const auto inspection = workspace.InspectSelectedObjects(selected->fence).Value();
            const auto &row = inspection->objects.front();
            REQUIRE(row.source.descriptor.bounds == object.descriptor.bounds);
            REQUIRE(row.automaticCells == expected);
            REQUIRE(row.placementCells == expected);
            REQUIRE(row.spanning == (expected.size() > 1));
            REQUIRE_FALSE(row.requiresDirective);
            REQUIRE_FALSE(row.diagnostic);
        }

        TEST_CASE("World object inspector applies canonical spanning overrides without changing automatic coverage",
                  "[unit][editor][world_document][object_inspection]") {
            auto document = InspectionDocument();
            auto &object = document.pages[0].objects[0];
            object.descriptor.bounds = Bounds(-1, 1000);
            object.spanning = WS::WorldSpanningObjectDirective{object.descriptor.address, object.descriptor.revision,
                                                               WS::WorldSpanningObjectPolicy::SingleCellOwner, Cell(0)};
            auto expected = WS::WorldSpanningObjectPlacement::SingleCellOwner;
            std::vector<WS::StreamingCellId> effective{Cell(0)};
            SECTION("one owner within coverage") {}
            SECTION("split per cell") {
                object.spanning->policy = WS::WorldSpanningObjectPolicy::SplitPerCell;
                object.spanning->ownerCell = {};
                expected = WS::WorldSpanningObjectPlacement::SplitPerCell;
                effective = {Cell(-1), Cell(0), Cell(1)};
            }
            SECTION("explicit nonspatial") {
                object.spanning->policy = WS::WorldSpanningObjectPolicy::NonSpatial;
                object.spanning->ownerCell = {};
                expected = WS::WorldSpanningObjectPlacement::NonSpatial;
                effective.clear();
            }
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            const auto selected = SelectFirst(workspace, document);
            const auto result = workspace.InspectSelectedObjects(selected->fence).Value();
            const auto &row = result->objects.front();
            REQUIRE(row.automaticCells == std::vector{Cell(-1), Cell(0), Cell(1)});
            REQUIRE(row.placementCells == effective);
            REQUIRE(row.source.spanning == object.spanning);
            REQUIRE(row.placement == expected);
            REQUIRE(row.spanning);
            REQUIRE(row.requiresDirective);
            REQUIRE_FALSE(row.diagnostic);
            REQUIRE(workspace.Snapshot().Value() == selected);
        }

        TEST_CASE("World object inspector retains actionable validation rather than inventing an effective placement",
                  "[unit][editor][world_document][object_inspection]") {
            auto document = InspectionDocument();
            auto &object = document.pages[0].objects[0];
            object.descriptor.bounds = Bounds(-1, 1000);
            const ErrorCodeDescriptor *expected = &WS::WorldStreamingErrors::SpanningObjectPlanInvalid;
            SECTION("missing spanning decision") {}
            SECTION("owner outside automatic coverage") {
                object.spanning = WS::WorldSpanningObjectDirective{object.descriptor.address, object.descriptor.revision,
                                                                   WS::WorldSpanningObjectPolicy::SingleCellOwner, Cell(2)};
                expected = &WS::WorldStreamingErrors::SpanningObjectPlanOwnerUnavailable;
            }
            SECTION("override on a direct object") {
                object.descriptor.bounds = Bounds(0, 99);
                object.spanning = WS::WorldSpanningObjectDirective{object.descriptor.address,
                                                                   object.descriptor.revision,
                                                                   WS::WorldSpanningObjectPolicy::NonSpatial,
                                                                   {}};
            }
            SECTION("source bounds outside authored world") {
                object.descriptor.bounds = Bounds(3000, 4000);
                expected = &WS::WorldStreamingErrors::SpatialAssignmentInvalid;
            }
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            const auto selected = SelectFirst(workspace, document);
            const auto result = workspace.InspectSelectedObjects(selected->fence).Value();
            const auto &row = result->objects.front();
            REQUIRE(row.diagnostic);
            REQUIRE(row.diagnostic->code == expected->code);
            REQUIRE_FALSE(row.placement);
            REQUIRE(row.placementCells.empty());
            REQUIRE(row.source.descriptor.bounds == object.descriptor.bounds);
            REQUIRE(row.source.spanning == object.spanning);
        }

        TEST_CASE("Always present world objects explain nonspatial placement and empty selection is valid",
                  "[unit][editor][world_document][object_inspection]") {
            auto document = InspectionDocument();
            document.pages[0].objects[0].descriptor.placement = WS::WorldSpatialObjectPlacementClass::AlwaysPresent;
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            const auto selected = SelectFirst(workspace, document);
            const auto row = workspace.InspectSelectedObjects(selected->fence).Value()->objects.front();
            REQUIRE(row.placement == WS::WorldSpanningObjectPlacement::NonSpatial);
            REQUIRE(row.automaticCells.empty());
            REQUIRE(row.placementCells.empty());
            REQUIRE_FALSE(row.spanning);
            REQUIRE_FALSE(row.diagnostic);
            const auto cleared = workspace.SetSelection({}, std::nullopt, selected->fence).Value();
            REQUIRE_FALSE(cleared->dirty);
            REQUIRE(workspace.InspectSelectedObjects(cleared->fence).Value()->objects.empty());
        }

        TEST_CASE("World selection and inspection limits reject complete requests without partial hidden state",
                  "[unit][editor][world_document][object_inspection]") {
            auto document = InspectionDocument();
            PartitionedWorldWorkspace workspace{InspectionAsset(9), {.maximumSelectedObjects = 1}};
            const auto selected = SelectFirst(workspace, document);
            SECTION("selection capacity") {
                const auto objects =
                    std::array{document.pages[0].objects[0].descriptor.address, document.pages[1].objects[0].descriptor.address};
                InspectionError(workspace.SetSelection(objects, std::nullopt, selected->fence), WorldDocumentErrors::Capacity);
            }
            SECTION("aggregate includes automatic and effective cells") {
                InspectionError(workspace.InspectSelectedObjects(selected->fence, {1, 1}), WorldDocumentErrors::Capacity);
                REQUIRE(workspace.InspectSelectedObjects(selected->fence, {1, 2}).HasValue());
            }
            SECTION("zero budget") {
                InspectionError(workspace.InspectSelectedObjects(selected->fence, {0, 10}), WorldDocumentErrors::Capacity);
            }
            SECTION("raised hard budget") {
                InspectionError(workspace.InspectSelectedObjects(selected->fence, {4097, 65536}), WorldDocumentErrors::Capacity);
            }
            REQUIRE(workspace.Snapshot().Value() == selected);
            REQUIRE(workspace.InspectSelectedObjects(selected->fence).HasValue());
        }

        TEST_CASE("World selection rejects duplicate unknown invalid foreign and stale identities atomically",
                  "[unit][editor][world_document][object_inspection]") {
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            const auto document = InspectionDocument();
            const auto selected = SelectFirst(workspace, document);
            const auto address = document.pages[0].objects[0].descriptor.address;
            SECTION("duplicate") {
                InspectionError(workspace.SetSelection(std::array{address, address}, std::nullopt, selected->fence),
                                WorldDocumentErrors::Conflict);
            }
            SECTION("invalid") {
                InspectionError(workspace.SetSelection(std::array{WS::WorldAuthoringObjectAddress{}}, std::nullopt, selected->fence),
                                WorldDocumentErrors::Invalid);
            }
            SECTION("unknown page scoped address") {
                auto unknown = address;
                unknown.page = InspectionAsset(8);
                InspectionError(workspace.SetSelection(std::array{unknown}, std::nullopt, selected->fence), WorldDocumentErrors::Conflict);
            }
            SECTION("primary outside selection") {
                InspectionError(workspace.SetSelection(std::array{address}, document.pages[1].objects[0].descriptor.address,
                                                       selected->fence),
                                WorldDocumentErrors::Conflict);
            }
            SECTION("foreign fence") {
                auto fence = selected->fence;
                fence.owner = InspectionAsset(8);
                InspectionError(workspace.SetSelection({}, std::nullopt, fence), WorldDocumentErrors::Stale);
                InspectionError(workspace.InspectSelectedObjects(fence), WorldDocumentErrors::Stale);
            }
            REQUIRE(workspace.Snapshot().Value() == selected);
        }

        TEST_CASE("World source overrides reject unsupported stale and malformed metadata before owner publication",
                  "[unit][editor][world_document][object_inspection]") {
            auto document = InspectionDocument();
            auto &object = document.pages[0].objects[0];
            object.spanning = WS::WorldSpanningObjectDirective{object.descriptor.address, object.descriptor.revision,
                                                               WS::WorldSpanningObjectPolicy::SingleCellOwner, Cell(0)};
            const ErrorCodeDescriptor *expected = &WorldDocumentErrors::Invalid;
            SECTION("unsupported override") {
                object.spanning->policy = static_cast<WS::WorldSpanningObjectPolicy>(255);
                expected = &WorldDocumentErrors::Unsupported;
            }
            SECTION("stale directive revision") {
                object.spanning->revision = WS::WorldAuthoringRevision::Create(2).Value();
            }
            SECTION("foreign directive address") {
                object.spanning->address.page = InspectionAsset(8);
            }
            SECTION("missing required owner") {
                object.spanning->ownerCell = {};
            }
            SECTION("always present cannot have spanning override") {
                object.descriptor.placement = WS::WorldSpatialObjectPlacementClass::AlwaysPresent;
            }
            SECTION("unsupported assignment LOD") {
                object.assignmentLod = 2;
                expected = &WorldDocumentErrors::Unsupported;
            }
            SECTION("zero direct cell threshold") {
                document.settings.maximumDirectCellsPerObject = 0;
            }
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            InspectionError(workspace.Open(document), *expected);
            InspectionError(workspace.Snapshot(), WorldDocumentErrors::Lifecycle);
        }

        TEST_CASE("Source override edits publish once through the world owner and preserve selected stable identities",
                  "[unit][editor][world_document][object_inspection]") {
            auto document = InspectionDocument();
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            const auto selected = SelectFirst(workspace, document);
            const auto before = workspace.InspectSelectedObjects(selected->fence).Value();
            auto &page = document.pages[0];
            page.head.revision = WS::WorldAuthoringRevision::Create(2).Value();
            auto &object = page.objects[0];
            object.descriptor.revision = page.head.revision;
            object.descriptor.bounds = Bounds(-1, 1000);
            object.spanning = WS::WorldSpanningObjectDirective{object.descriptor.address, object.descriptor.revision,
                                                               WS::WorldSpanningObjectPolicy::SingleCellOwner, Cell(1)};
            const auto committed = workspace.Commit(document, selected->fence).Value();
            REQUIRE(committed->dirty);
            REQUIRE(committed->selectedObjects == selected->selectedObjects);
            REQUIRE(committed->fence.revision.Value() == selected->fence.revision.Value() + 1);
            const auto after = workspace.InspectSelectedObjects(committed->fence).Value();
            REQUIRE(after->objects.front().placementCells == std::vector{Cell(1)});
            REQUIRE(before->objects.front().placementCells == std::vector{Cell(0)});
            document.settings.maximumDirectCellsPerObject = 0;
            InspectionError(workspace.Commit(document, committed->fence), WorldDocumentErrors::Invalid);
            REQUIRE(workspace.Snapshot().Value() == committed);
        }

        TEST_CASE("World source removal prunes selection in the same revision and retained inspections survive replacement",
                  "[unit][editor][world_document][object_inspection]") {
            auto document = InspectionDocument();
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            const auto selected = SelectFirst(workspace, document);
            const auto retained = workspace.InspectSelectedObjects(selected->fence).Value();
            document.pages[0].head.revision = WS::WorldAuthoringRevision::Create(2).Value();
            document.pages[0].objects.clear();
            const auto committed = workspace.Commit(document, selected->fence).Value();
            REQUIRE(committed->selectedObjects.empty());
            REQUIRE_FALSE(committed->primary);
            REQUIRE(committed->dirty);
            InspectionError(workspace.InspectSelectedObjects(selected->fence), WorldDocumentErrors::Stale);
            const auto replacement = workspace.Replace(InspectionDocument(), committed->fence, WorldDocumentDisposition::Discard).Value();
            REQUIRE(replacement->selectedObjects.empty());
            REQUIRE(replacement->fence.generation != selected->fence.generation);
            InspectionError(workspace.InspectSelectedObjects(committed->fence), WorldDocumentErrors::Stale);
            REQUIRE(workspace.Shutdown().HasValue());
            InspectionError(workspace.InspectSelectedObjects(replacement->fence), WorldDocumentErrors::Lifecycle);
            REQUIRE(retained->objects.front().placementCells == std::vector{Cell(0)});
            REQUIRE(retained->fence == selected->fence);
        }

        TEST_CASE("World inspector cancellation and wrong thread leave selection and document intact",
                  "[unit][editor][world_document][object_inspection]") {
            PartitionedWorldWorkspace workspace{InspectionAsset(9)};
            const auto selected = SelectFirst(workspace, InspectionDocument());
            std::stop_source stopped;
            stopped.request_stop();
            InspectionError(workspace.SetSelection({}, std::nullopt, selected->fence, stopped.get_token()), WorldDocumentErrors::Cancelled);
            InspectionError(workspace.InspectSelectedObjects(selected->fence, {}, stopped.get_token()), WorldDocumentErrors::Cancelled);
            bool wrongSelection{};
            bool wrongInspection{};
            std::thread worker([&] {
                const auto selection = workspace.SetSelection({}, std::nullopt, selected->fence);
                wrongSelection = selection.HasError() && selection.ErrorValue().code == WorldDocumentErrors::WrongThread.code;
                const auto inspection = workspace.InspectSelectedObjects(selected->fence);
                wrongInspection = inspection.HasError() && inspection.ErrorValue().code == WorldDocumentErrors::WrongThread.code;
            });
            worker.join();
            REQUIRE(wrongSelection);
            REQUIRE(wrongInspection);
            REQUIRE(workspace.Snapshot().Value() == selected);
            REQUIRE(workspace.BeginCancellation().HasValue());
            InspectionError(workspace.SetSelection({}, std::nullopt, selected->fence), WorldDocumentErrors::Lifecycle);
            InspectionError(workspace.InspectSelectedObjects(selected->fence), WorldDocumentErrors::Lifecycle);
            REQUIRE(workspace.Snapshot().Value() == selected);
            REQUIRE(workspace.Shutdown().HasValue());
        }
    }  // namespace
}  // namespace Horo::Editor
