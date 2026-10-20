#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        Assets::AssetId Asset(const std::uint8_t value = 1) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return Assets::AssetId::FromBytes(bytes);
        }

        PartitionedWorldDocument Document() {
            const auto world = WS::WorldPartitionId::Create(Asset().Bytes()).Value();
            const auto grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-1, 1, -1, 1, -1, 1}, 1).Value();
            PartitionedWorldDocument document{.settings = {.partition = world, .bounds = {}, .grid = grid}};
            document.layers.push_back({.id = WS::StreamingLayerId::Create(0).Value(), .name = "Terrain"});
            document.pages.push_back(
                {.head = {.partition = world, .sourceAsset = Asset(2), .revision = WS::WorldAuthoringRevision::Create(1).Value()}});
            return document;
        }

        template <typename T> void RequireError(const Result<T> &result, const std::string_view code) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == code);
        }

        PartitionedWorldObject Object(const std::uint64_t object = 1) {
            return {.descriptor = {.address = {.page = Asset(2), .object = object},
                                   .revision = WS::WorldAuthoringRevision::Create(1).Value(),
                                   .sourceAsset = Asset(3)},
                    .layer = WS::StreamingLayerId::Create(0).Value()};
        }

        TEST_CASE("Layer outliner commands preserve stable identities and exact undo redo state",
                  "[unit][editor][world_document][layers]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto current = workspace.Open(Document()).Value();
            const auto initial = current;
            const auto id = WS::StreamingLayerId::Create(7).Value();
            const WS::WorldLayerDescriptor layer{.id = id, .name = "Gameplay", .ownership = WS::WorldLayerOwnership::GameplayScript};
            current = workspace.ExecuteLayerCommand(CreateWorldLayer{layer, 0}, current->fence).Value();
            REQUIRE(current->document.layers.front() == layer);
            REQUIRE(current->dirty);
            REQUIRE(current->layerUndoCount == 1);
            REQUIRE(initial->document.layers.size() == 1);
            current = workspace.ExecuteLayerCommand(SetCurrentWorldLayer{id}, current->fence).Value();
            current = workspace.ExecuteLayerCommand(FilterWorldLayers{{"Terrain", {}}}, current->fence).Value();
            REQUIRE(current->currentLayer == id);
            REQUIRE(workspace.VisibleLayers().Value() == std::vector{WS::StreamingLayerId::Create(0).Value()});
            current = workspace.ExecuteLayerCommand(ReorderWorldLayer{id, 1}, current->fence).Value();
            REQUIRE(current->document.layers.back().id == id);
            REQUIRE(current->layerUndoCount == 4);
            for (int index = 0; index < 4; ++index)
                current = workspace.ReplayLayerCommand(false, current->fence).Value();
            REQUIRE(current->document.layers == initial->document.layers);
            REQUIRE_FALSE(current->currentLayer.has_value());
            REQUIRE(current->layerFilter == WorldLayerOutlinerFilter{});
            REQUIRE_FALSE(current->dirty);
            REQUIRE(current->layerRedoCount == 4);
            REQUIRE(current->fence.revision.Value() > initial->fence.revision.Value());
            for (int index = 0; index < 4; ++index)
                current = workspace.ReplayLayerCommand(true, current->fence).Value();
            REQUIRE(current->currentLayer == id);
            REQUIRE(current->document.layers.back() == layer);
            REQUIRE(current->dirty);
        }

        TEST_CASE("Rejected layer commands preserve document context and history atomically", "[unit][editor][world_document][layers]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto current = workspace.Open(Document()).Value();
            current = workspace.ExecuteLayerCommand(SetCurrentWorldLayer{current->document.layers.front().id}, current->fence).Value();
            WorldLayerCommand command = ReorderWorldLayer{WS::StreamingLayerId::Create(99).Value(), 0};
            std::string_view code = "world_document.invalid";
            SECTION("missing layer") {}
            SECTION("invalid layer") {
                command = SetCurrentWorldLayer{WS::StreamingLayerId{}};
            }
            SECTION("out of range move") {
                command = ReorderWorldLayer{current->document.layers.front().id, 1};
            }
            SECTION("out of range insertion") {
                command = CreateWorldLayer{{.id = WS::StreamingLayerId::Create(2).Value(), .name = "New"}, 2};
            }
            SECTION("duplicate identity") {
                command = CreateWorldLayer{current->document.layers.front(), 0};
                code = "world_document.conflict";
            }
            SECTION("unsupported metadata") {
                command = CreateWorldLayer{{.id = WS::StreamingLayerId::Create(2).Value(),
                                            .name = "New",
                                            .flags = static_cast<WS::WorldLayerFlags>(128)},
                                           0};
                code = "world_document.unsupported";
            }
            SECTION("empty name") {
                command = CreateWorldLayer{{.id = WS::StreamingLayerId::Create(2).Value()}, 0};
            }
            SECTION("invalid filter UTF8") {
                command = FilterWorldLayers{{"\xFF", {}}};
            }
            SECTION("filter embedded NUL") {
                command = FilterWorldLayers{{std::string("a\0b", 3), {}}};
            }
            SECTION("unsupported filter ownership") {
                command = FilterWorldLayers{{{}, static_cast<WS::WorldLayerOwnership>(255)}};
                code = "world_document.unsupported";
            }
            SECTION("filter capacity") {
                command = FilterWorldLayers{{std::string(1025, 'a'), {}}};
                code = "world_document.capacity";
            }
            SECTION("layer name capacity") {
                command = CreateWorldLayer{{.id = WS::StreamingLayerId::Create(2).Value(), .name = std::string(65537, 'a')}, 0};
                code = "world_document.capacity";
            }
            RequireError(workspace.ExecuteLayerCommand(command, current->fence), code);
            REQUIRE(workspace.Snapshot().Value() == current);
            REQUIRE(current->layerUndoCount == 1);
            current = workspace.ReplayLayerCommand(false, current->fence).Value();
            REQUIRE_FALSE(current->currentLayer.has_value());
        }

        TEST_CASE("Layer filters and authoring context are transient and support ownership and non ASCII names",
                  "[unit][editor][world_document][layers]") {
            auto document = Document();
            const auto id = WS::StreamingLayerId::Create(42).Value();
            document.layers.push_back({.id = id, .name = "Çevre", .ownership = WS::WorldLayerOwnership::GameplayScript});
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto current = workspace.Open(document).Value();
            current =
                workspace.ExecuteLayerCommand(FilterWorldLayers{{"Ç", WS::WorldLayerOwnership::GameplayScript}}, current->fence).Value();
            REQUIRE(workspace.VisibleLayers().Value() == std::vector{id});
            REQUIRE_FALSE(current->dirty);
            current = workspace.ExecuteLayerCommand(SetCurrentWorldLayer{id}, current->fence).Value();
            current = workspace.ExecuteLayerCommand(FilterWorldLayers{{"missing", {}}}, current->fence).Value();
            REQUIRE(workspace.VisibleLayers().Value().empty());
            REQUIRE(current->currentLayer == id);
            current = workspace.ExecuteLayerCommand(SetCurrentWorldLayer{std::nullopt}, current->fence).Value();
            REQUIRE_FALSE(current->currentLayer.has_value());
            REQUIRE_FALSE(current->dirty);
            REQUIRE(current->document.layers == document.layers);
        }

        TEST_CASE("Layer history rejects stale cancelled and worker operations and preserves working sets",
                  "[unit][editor][world_document][layers]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            const auto initial = workspace.Open(Document()).Value();
            const auto command = SetCurrentWorldLayer{initial->document.layers.front().id};
            auto current = workspace.ExecuteLayerCommand(command, initial->fence).Value();
            RequireError(workspace.ExecuteLayerCommand(command, initial->fence), "world_document.stale");
            RequireError(workspace.ReplayLayerCommand(false, initial->fence), "world_document.stale");
            std::stop_source cancelled;
            cancelled.request_stop();
            RequireError(workspace.ExecuteLayerCommand(command, current->fence, cancelled.get_token()), "world_document.cancelled");
            RequireError(workspace.ReplayLayerCommand(false, current->fence, cancelled.get_token()), "world_document.cancelled");
            REQUIRE(workspace.Snapshot().Value() == current);
            std::vector<std::string> codes;
            std::thread worker([&] {
                codes.push_back(workspace.ExecuteLayerCommand(command, current->fence).ErrorValue().code.Value());
                codes.push_back(workspace.ReplayLayerCommand(false, current->fence).ErrorValue().code.Value());
                codes.push_back(workspace.VisibleLayers().ErrorValue().code.Value());
            });
            worker.join();
            REQUIRE(codes == std::vector<std::string>(3, "world_document.wrong_thread"));
            const std::array pages{Asset(2)};
            current = workspace.SetWorkingSet(pages, current->fence).Value();
            current = workspace.ReplayLayerCommand(false, current->fence).Value();
            REQUIRE(current->openPages == std::vector{Asset(2)});
            REQUIRE_FALSE(current->currentLayer.has_value());
        }

        TEST_CASE("Layer history no ops preserve redo and a new command drops redo", "[unit][editor][world_document][layers]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto current = workspace.Open(Document()).Value();
            RequireError(workspace.ReplayLayerCommand(false, current->fence), "world_document.conflict");
            current = workspace.ExecuteLayerCommand(FilterWorldLayers{{"Terrain", {}}}, current->fence).Value();
            current = workspace.ReplayLayerCommand(false, current->fence).Value();
            REQUIRE(workspace.ExecuteLayerCommand(FilterWorldLayers{}, current->fence).Value() == current);
            REQUIRE(workspace.ExecuteLayerCommand(ReorderWorldLayer{current->document.layers.front().id, 0}, current->fence).Value() ==
                    current);
            REQUIRE(current->layerRedoCount == 1);
            current = workspace.ExecuteLayerCommand(SetCurrentWorldLayer{current->document.layers.front().id}, current->fence).Value();
            REQUIRE(current->layerRedoCount == 0);
            RequireError(workspace.ReplayLayerCommand(true, current->fence), "world_document.conflict");
        }

        TEST_CASE("Layer creation enforces exact layer and aggregate name capacity", "[unit][editor][world_document][layers]") {
            WorldDocumentLimits limits;
            std::string name = "New";
            SECTION("layer count") {
                limits.maximumLayers = 1;
            }
            SECTION("aggregate name bytes") {
                limits.maximumLayerNameBytes = 8;
            }
            PartitionedWorldWorkspace workspace{Asset(9), limits};
            const auto current = workspace.Open(Document()).Value();
            RequireError(workspace.ExecuteLayerCommand(CreateWorldLayer{{.id = WS::StreamingLayerId::Create(2).Value(), .name = name}, 1},
                                                       current->fence),
                         "world_document.capacity");
            REQUIRE(workspace.Snapshot().Value() == current);
        }

        TEST_CASE("Layer history byte ceiling evicts complete transactions before entry count ceiling",
                  "[unit][editor][world_document][layers]") {
            auto document = Document();
            document.layers.front().name = std::string(65536, 'a');
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto current = workspace.Open(document).Value();
            for (int index = 0; index < 40; ++index)
                current = workspace.ExecuteLayerCommand(FilterWorldLayers{{std::to_string(index), {}}}, current->fence).Value();
            REQUIRE(current->layerUndoCount > 0);
            REQUIRE(current->layerUndoCount < 40);
            const auto retained = current->layerUndoCount;
            for (std::size_t index = 0; index < retained; ++index)
                current = workspace.ReplayLayerCommand(false, current->fence).Value();
            RequireError(workspace.ReplayLayerCommand(false, current->fence), "world_document.conflict");
            REQUIRE(current->document.layers == document.layers);
            REQUIRE_FALSE(current->dirty);
        }

        TEST_CASE("Layer history is bounded and reset by external commit replacement and close", "[unit][editor][world_document][layers]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto current = workspace.Open(Document()).Value();
            for (int index = 0; index < 140; ++index)
                current = workspace.ExecuteLayerCommand(FilterWorldLayers{{std::to_string(index), {}}}, current->fence).Value();
            REQUIRE(current->layerUndoCount == 128);
            SECTION("metadata commit clears history and prunes missing current identity") {
                current = workspace.ExecuteLayerCommand(SetCurrentWorldLayer{current->document.layers.front().id}, current->fence).Value();
                auto document = current->document;
                document.layers.clear();
                current = workspace.Commit(document, current->fence).Value();
                REQUIRE_FALSE(current->currentLayer.has_value());
                REQUIRE(current->layerUndoCount == 0);
            }
            SECTION("replacement clears history and context") {
                current = workspace.Replace(Document(), current->fence).Value();
                REQUIRE(current->layerFilter.name.empty());
                REQUIRE(current->layerUndoCount == 0);
            }
            SECTION("close reopen clears history") {
                REQUIRE(workspace.Close(current->fence).HasValue());
                current = workspace.Open(Document()).Value();
                REQUIRE(current->layerUndoCount == 0);
            }
            SECTION("cancellation gates mutations but keeps projection and retained lease") {
                REQUIRE(workspace.BeginCancellation().HasValue());
                RequireError(workspace.ExecuteLayerCommand(FilterWorldLayers{}, current->fence), "world_document.lifecycle");
                RequireError(workspace.ReplayLayerCommand(false, current->fence), "world_document.lifecycle");
                REQUIRE(workspace.VisibleLayers().HasValue());
                REQUIRE(workspace.Shutdown().HasValue());
                REQUIRE(current->layerUndoCount == 128);
                RequireError(workspace.VisibleLayers(), "world_document.lifecycle");
                return;
            }
            RequireError(workspace.ReplayLayerCommand(false, current->fence), "world_document.conflict");
            REQUIRE(current->layerRedoCount == 0);
        }

        TEST_CASE("Partitioned world admission rejects malformed document metadata without publishing partial state",
                  "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            document.pages.front().objects.push_back(Object());
            std::string_view error = "world_document.invalid";
            SECTION("invalid partition") {
                document.settings.partition = {};
            }
            SECTION("unsupported document version") {
                document.version = 2;
                error = "world_document.unsupported";
            }
            RequireError(workspace.Open(document), error);
            RequireError(workspace.Snapshot(), "world_document.lifecycle");
        }

        TEST_CASE("Partitioned world admission rejects malformed layers without publishing partial state",
                  "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            document.pages.front().objects.push_back(Object());
            std::string_view error = "world_document.invalid";
            SECTION("duplicate layer identity") {
                document.layers.push_back(document.layers.front());
                error = "world_document.conflict";
            }
            SECTION("invalid layer identity") {
                document.layers.front().id = {};
            }
            SECTION("empty layer name") {
                document.layers.front().name.clear();
            }
            SECTION("invalid UTF8 layer name") {
                document.layers.front().name = "\xFF";
            }
            SECTION("embedded NUL layer name") {
                document.layers.front().name = std::string("a\0b", 3);
            }
            SECTION("unsupported ownership") {
                document.layers.front().ownership = static_cast<WS::WorldLayerOwnership>(255);
                error = "world_document.unsupported";
            }
            SECTION("unsupported flags") {
                document.layers.front().flags = static_cast<WS::WorldLayerFlags>(128);
                error = "world_document.unsupported";
            }
            SECTION("contradictory flags") {
                document.layers.front().flags = WS::WorldLayerFlags::ServerOnly | WS::WorldLayerFlags::ClientOnly;
            }
            SECTION("invalid priority") {
                document.layers.front().priorityMultiplier = 0;
            }
            RequireError(workspace.Open(document), error);
            RequireError(workspace.Snapshot(), "world_document.lifecycle");
        }

        TEST_CASE("Partitioned world admission rejects malformed pages and objects without publishing partial state",
                  "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            document.pages.front().objects.push_back(Object());
            std::string_view error = "world_document.invalid";
            SECTION("wrong page partition") {
                document.pages.front().head.partition = WS::WorldPartitionId::Create(Asset(4).Bytes()).Value();
                error = "world_document.conflict";
            }
            SECTION("duplicate page identity") {
                document.pages.push_back(document.pages.front());
                error = "world_document.conflict";
            }
            SECTION("invalid page revision") {
                document.pages.front().head.revision = {};
            }
            SECTION("wrong object page") {
                document.pages.front().objects.front().descriptor.address.page = Asset(4);
                error = "world_document.conflict";
            }
            SECTION("object revision differs from page") {
                document.pages.front().objects.front().descriptor.revision = WS::WorldAuthoringRevision::Create(2).Value();
                error = "world_document.conflict";
            }
            SECTION("unknown object layer") {
                document.pages.front().objects.front().layer = WS::StreamingLayerId::Create(1).Value();
                error = "world_document.conflict";
            }
            SECTION("duplicate object address") {
                document.pages.front().objects.push_back(Object());
                error = "world_document.conflict";
            }
            SECTION("unsupported object version") {
                document.pages.front().objects.front().descriptor.version.major = 2;
                error = WS::WorldStreamingErrors::SpatialObjectVersionUnsupported.code.Value();
            }
            RequireError(workspace.Open(document), error);
            RequireError(workspace.Snapshot(), "world_document.lifecycle");
        }

        TEST_CASE("Partitioned world admission enforces metadata ceilings before ownership", "[unit][editor][world_document]") {
            WorldDocumentLimits limits;
            auto document = Document();
            document.pages.front().objects.push_back(Object());
            SECTION("layer count") {
                limits.maximumLayers = 1;
                document.layers.push_back({.id = WS::StreamingLayerId::Create(1).Value(), .name = "Extra"});
            }
            SECTION("page count") {
                limits.maximumPages = 1;
                auto page = document.pages.front();
                page.head.sourceAsset = Asset(4);
                page.objects.clear();
                document.pages.push_back(page);
            }
            SECTION("object count") {
                limits.maximumObjects = 1;
                document.pages.front().objects.push_back(Object(2));
            }
            SECTION("layer name bytes") {
                limits.maximumLayerNameBytes = 2;
            }
            PartitionedWorldWorkspace workspace{Asset(9), limits};
            RequireError(workspace.Open(document), "world_document.capacity");
            RequireError(workspace.Snapshot(), "world_document.lifecycle");
        }

        TEST_CASE("Partitioned world operations reject foreign owners and worker-thread writes", "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            const auto current = workspace.Open(document).Value();
            auto foreign = current->fence;
            foreign.owner = Asset(8);
            RequireError(workspace.Commit(document, foreign), "world_document.stale");
            RequireError(workspace.SetWorkingSet({}, foreign), "world_document.stale");
            RequireError(workspace.Replace(document, foreign), "world_document.stale");
            RequireError(workspace.Close(foreign), "world_document.stale");
            const auto expected = std::string("world_document.wrong_thread");
            std::vector<std::string> codes;
            std::thread worker([&] {
                codes.push_back(workspace.Open(document).ErrorValue().code.Value());
                codes.push_back(workspace.Snapshot().ErrorValue().code.Value());
                codes.push_back(workspace.Commit(document, current->fence).ErrorValue().code.Value());
                codes.push_back(workspace.SetWorkingSet({}, current->fence).ErrorValue().code.Value());
                codes.push_back(workspace.Replace(document, current->fence).ErrorValue().code.Value());
                codes.push_back(workspace.Close(current->fence).ErrorValue().code.Value());
                codes.push_back(workspace.BeginCancellation().ErrorValue().code.Value());
                codes.push_back(workspace.Shutdown().ErrorValue().code.Value());
            });
            worker.join();
            REQUIRE(codes == std::vector<std::string>(8, expected));
            REQUIRE(workspace.Snapshot().Value() == current);
        }

        TEST_CASE("Partitioned world immutable snapshots survive destruction and page revision exhaustion",
                  "[unit][editor][world_document]") {
            PartitionedWorldSnapshotLease retained;
            {
                PartitionedWorldWorkspace workspace{Asset(9)};
                auto document = Document();
                document.pages.front().head.revision =
                    WS::WorldAuthoringRevision::Create(std::numeric_limits<std::uint64_t>::max()).Value();
                retained = workspace.Open(document).Value();
                document.pages.front().objects.push_back(Object());
                document.pages.front().objects.front().descriptor.revision = document.pages.front().head.revision;
                RequireError(workspace.Commit(document, retained->fence), WS::WorldStreamingErrors::GenerationExhausted.code.Value());
                REQUIRE(workspace.Snapshot().Value() == retained);
            }
            REQUIRE(retained->document.pages.size() == 1);
            REQUIRE(retained->document.pages.front().objects.empty());
        }

        TEST_CASE("Partitioned world empty source and exact capacities require no cooked cells or payloads",
                  "[unit][editor][world_document]") {
            auto document = Document();
            SECTION("empty authored world") {
                document.layers.clear();
                document.pages.clear();
                PartitionedWorldWorkspace workspace{Asset(9)};
                REQUIRE(workspace.Open(document).HasValue());
            }
            SECTION("exact metadata capacities and duplicate working set rejection") {
                document.pages.front().objects.push_back(Object());
                PartitionedWorldWorkspace workspace{Asset(9),
                                                    {.maximumLayers = 1,
                                                     .maximumPages = 1,
                                                     .maximumObjects = 1,
                                                     .maximumLayerNameBytes = 7,
                                                     .maximumOpenPages = 2}};
                const auto current = workspace.Open(document).Value();
                const std::array duplicates{Asset(2), Asset(2)};
                RequireError(workspace.SetWorkingSet(duplicates, current->fence), "world_document.conflict");
                const std::array invalid{Assets::AssetId{}};
                RequireError(workspace.SetWorkingSet(invalid, current->fence), "world_document.conflict");
                REQUIRE(workspace.Snapshot().Value() == current);
            }
            SECTION("unknown disposition is never discard") {
                PartitionedWorldWorkspace workspace{Asset(9)};
                const auto current = workspace.Open(document).Value();
                const auto unknown = static_cast<WorldDocumentDisposition>(255);
                RequireError(workspace.Replace(document, current->fence, unknown), "world_document.unsupported");
                RequireError(workspace.Close(current->fence, unknown), "world_document.unsupported");
                RequireError(workspace.Shutdown(unknown), "world_document.unsupported");
                REQUIRE(workspace.Snapshot().Value() == current);
            }
        }

        TEST_CASE("Partitioned world transactions preserve page-scoped object identity across page additions",
                  "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            document.pages.front().objects.push_back(Object());
            const auto initial = workspace.Open(document).Value();
            auto page = document.pages.front();
            page.head.sourceAsset = Asset(4);
            page.objects.front().descriptor.address.page = page.head.sourceAsset;
            document.pages.push_back(page);
            const auto committed = workspace.Commit(document, initial->fence);
            REQUIRE(committed.HasValue());
            const auto current = committed.Value();
            REQUIRE(current->document.pages.size() == 2);
            const auto &first = current->document.pages.front().objects.front().descriptor;
            const auto &second = current->document.pages.back().objects.front().descriptor;
            REQUIRE(first.address.object == second.address.object);
            REQUIRE(first.sourceAsset == second.sourceAsset);
            REQUIRE(first.address != second.address);
            REQUIRE(initial->document.pages.size() == 1);
            auto otherWorld = document;
            otherWorld.settings.partition = WS::WorldPartitionId::Create(Asset(5).Bytes()).Value();
            for (auto &otherPage : otherWorld.pages)
                otherPage.head.partition = otherWorld.settings.partition;
            RequireError(workspace.Commit(otherWorld, current->fence), "world_document.conflict");
            REQUIRE(workspace.Snapshot().Value() == current);
        }

        TEST_CASE("Partitioned world settings require ordered content bounds inside the authored grid", "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            const auto current = workspace.Open(document).Value();
            std::string_view code = "world_document.invalid";
            SECTION("inverted X") {
                document.settings.bounds.minimum = Math::WorldCoordinate64::FromMillimeters(1, 0, 0);
            }
            SECTION("inverted Y") {
                document.settings.bounds.minimum = Math::WorldCoordinate64::FromMillimeters(0, 1, 0);
            }
            SECTION("inverted Z") {
                document.settings.bounds.minimum = Math::WorldCoordinate64::FromMillimeters(0, 0, 1);
            }
            SECTION("outside grid") {
                document.settings.bounds.maximum = Math::WorldCoordinate64::FromMillimeters(2000, 0, 0);
                code = WS::WorldStreamingErrors::CellOutOfBounds.code.Value();
            }
            RequireError(workspace.Commit(document, current->fence), code);
            REQUIRE(workspace.Snapshot().Value() == current);
        }

        TEST_CASE("Partitioned world owner rejects invalid identities and unbounded capacities", "[unit][editor][world_document]") {
            WorldDocumentLimits limits;
            auto owner = Asset(9);
            SECTION("invalid owner") {
                owner = {};
            }
            SECTION("zero layers") {
                limits.maximumLayers = 0;
            }
            SECTION("zero pages") {
                limits.maximumPages = 0;
            }
            SECTION("zero objects") {
                limits.maximumObjects = 0;
            }
            SECTION("zero name budget") {
                limits.maximumLayerNameBytes = 0;
            }
            SECTION("zero open pages") {
                limits.maximumOpenPages = 0;
            }
            SECTION("above hard layer ceiling") {
                ++limits.maximumLayers;
            }
            SECTION("above hard page ceiling") {
                ++limits.maximumPages;
            }
            SECTION("above hard object ceiling") {
                ++limits.maximumObjects;
            }
            SECTION("above hard name ceiling") {
                ++limits.maximumLayerNameBytes;
            }
            SECTION("above hard working set ceiling") {
                ++limits.maximumOpenPages;
            }
            PartitionedWorldWorkspace workspace{owner, limits};
            RequireError(workspace.Open(Document()), "world_document.invalid");
            RequireError(workspace.Snapshot(), "world_document.lifecycle");
        }

        TEST_CASE("Partitioned world cancellation never publishes or reports cancelled no-ops as success",
                  "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            std::stop_source cancelled;
            cancelled.request_stop();
            RequireError(workspace.Open(document, cancelled.get_token()), "world_document.cancelled");
            const auto current = workspace.Open(document).Value();
            RequireError(workspace.Commit(document, current->fence, cancelled.get_token()), "world_document.cancelled");
            RequireError(workspace.SetWorkingSet({}, current->fence, cancelled.get_token()), "world_document.cancelled");
            document.layers.front().name = "Cancelled";
            RequireError(workspace.Commit(document, current->fence, cancelled.get_token()), "world_document.cancelled");
            const std::array open{Asset(2)};
            RequireError(workspace.SetWorkingSet(open, current->fence, cancelled.get_token()), "world_document.cancelled");
            RequireError(workspace.Replace(document, current->fence, WorldDocumentDisposition::Preserve, cancelled.get_token()),
                         "world_document.cancelled");
            REQUIRE(workspace.Snapshot().Value() == current);
            REQUIRE(current->fence.generation.Value() == 1);
            REQUIRE(current->fence.revision.Value() == 1);
        }

        TEST_CASE("Partitioned world lifecycle preserves dirty ownership and invalidates replaced or reopened generations",
                  "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            const auto initial = workspace.Open(document).Value();
            RequireError(workspace.Open(document), "world_document.conflict");
            REQUIRE(workspace.Snapshot().Value() == initial);
            document.layers.front().name = "Dirty";
            const auto dirty = workspace.Commit(document, initial->fence).Value();
            RequireError(workspace.Close(dirty->fence), "world_document.dirty");
            RequireError(workspace.Replace(Document(), dirty->fence), "world_document.dirty");
            RequireError(workspace.Shutdown(), "world_document.dirty");
            REQUIRE(workspace.Snapshot().Value() == dirty);
            SECTION("explicit replacement invalidates old fences") {
                auto invalid = Document();
                invalid.version = 2;
                RequireError(workspace.Replace(invalid, dirty->fence, WorldDocumentDisposition::Discard), "world_document.unsupported");
                REQUIRE(workspace.Snapshot().Value() == dirty);
                const auto replaced = workspace.Replace(Document(), dirty->fence, WorldDocumentDisposition::Discard);
                REQUIRE(replaced.HasValue());
                REQUIRE(replaced.Value()->fence.generation.Value() > dirty->fence.generation.Value());
                REQUIRE_FALSE(replaced.Value()->dirty);
                REQUIRE(replaced.Value()->openPages.empty());
                RequireError(workspace.Commit(document, dirty->fence), "world_document.stale");
                RequireError(workspace.Close(dirty->fence, WorldDocumentDisposition::Discard), "world_document.stale");
            }
            SECTION("close and reopen cannot resurrect stale handles") {
                REQUIRE(workspace.Close(dirty->fence, WorldDocumentDisposition::Discard).HasValue());
                RequireError(workspace.Snapshot(), "world_document.lifecycle");
                const auto reopened = workspace.Open(Document()).Value();
                REQUIRE(reopened->fence.generation.Value() > dirty->fence.generation.Value());
                RequireError(workspace.SetWorkingSet({}, dirty->fence), "world_document.stale");
            }
        }

        TEST_CASE("Partitioned world cancellation and shutdown preserve readers while releasing owned state",
                  "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            const auto initial = workspace.Open(document).Value();
            document.layers.front().name = "Dirty";
            const auto dirty = workspace.Commit(document, initial->fence).Value();
            SECTION("cancellation closes admission but permits explicit teardown") {
                REQUIRE(workspace.BeginCancellation().HasValue());
                REQUIRE(workspace.BeginCancellation().HasValue());
                RequireError(workspace.Open(Document()), "world_document.lifecycle");
                RequireError(workspace.Commit(document, dirty->fence), "world_document.lifecycle");
                RequireError(workspace.Replace(Document(), dirty->fence, WorldDocumentDisposition::Discard), "world_document.lifecycle");
                RequireError(workspace.SetWorkingSet({}, dirty->fence), "world_document.lifecycle");
                REQUIRE(workspace.Snapshot().Value() == dirty);
                REQUIRE(workspace.Close(dirty->fence, WorldDocumentDisposition::Discard).HasValue());
                RequireError(workspace.Open(Document()), "world_document.lifecycle");
                REQUIRE(workspace.Shutdown().HasValue());
                REQUIRE(workspace.Shutdown().HasValue());
            }
            SECTION("shutdown releases owned state without invalidating readers") {
                REQUIRE(workspace.Shutdown(WorldDocumentDisposition::Discard).HasValue());
                RequireError(workspace.Snapshot(), "world_document.lifecycle");
                RequireError(workspace.Open(Document()), "world_document.lifecycle");
                REQUIRE(dirty->document.layers.front().name == "Dirty");
                REQUIRE(initial->document.layers.front().name == "Terrain");
            }
        }

        TEST_CASE("Partitioned world source transactions atomically edit settings layers and immutable page objects",
                  "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            const auto initial = workspace.Open(document).Value();
            REQUIRE(workspace.Commit(document, initial->fence).HasValue());
            REQUIRE(workspace.Snapshot().Value() == initial);
            document.layers.front().name = "Renamed stable layer";
            document.settings.grid = WS::WorldCellQuantizationPolicy::Create({}, 2000, {-1, 1, -1, 1, -1, 1}, 1).Value();
            auto &page = document.pages.front();
            page.head.revision = WS::WorldAuthoringRevision::Create(2).Value();
            page.objects.push_back(Object());
            page.objects.front().descriptor.revision = page.head.revision;
            const auto committed = workspace.Commit(document, initial->fence);
            REQUIRE(committed.HasValue());
            const auto current = committed.Value();
            REQUIRE(current->dirty);
            REQUIRE(current->fence.generation == initial->fence.generation);
            REQUIRE(current->fence.revision.Value() == initial->fence.revision.Value() + 1);
            REQUIRE(current->document.layers.front().id == initial->document.layers.front().id);
            REQUIRE(current->document.pages.front().objects.front().descriptor.address == Object().descriptor.address);
            REQUIRE(current->document.settings.grid.BaseCellSizeMillimeters() == 2000);
            REQUIRE(initial->document.pages.front().objects.empty());
            RequireError(workspace.Commit(document, initial->fence), "world_document.stale");
            auto invalid = document;
            invalid.pages.front().objects.clear();
            RequireError(workspace.Commit(invalid, current->fence), "world_document.stale");
            invalid = document;
            invalid.pages.front().head.revision = WS::WorldAuthoringRevision::Create(4).Value();
            invalid.pages.front().objects.front().descriptor.revision = invalid.pages.front().head.revision;
            RequireError(workspace.Commit(invalid, current->fence), "world_document.stale");
            invalid = document;
            invalid.layers.clear();
            RequireError(workspace.Commit(invalid, current->fence), "world_document.conflict");
            REQUIRE(workspace.Snapshot().Value() == current);
            const std::array open{Asset(2)};
            const auto working = workspace.SetWorkingSet(open, current->fence).Value();
            document.pages.clear();
            const auto removed = workspace.Commit(document, working->fence).Value();
            REQUIRE(removed->document.pages.empty());
            REQUIRE(removed->openPages.empty());
            REQUIRE(removed->dirty);
            REQUIRE(working->document.pages.size() == 1);
        }

        TEST_CASE("Partitioned world working set is bounded fenced and independent from source ownership",
                  "[unit][editor][world_document]") {
            auto document = Document();
            auto secondPage = document.pages.front();
            secondPage.head.sourceAsset = Asset(4);
            document.pages.push_back(secondPage);
            PartitionedWorldWorkspace workspace{Asset(9), {.maximumOpenPages = 1}};
            const auto initial = workspace.Open(document).Value();
            const std::array first{Asset(2)};
            const auto opened = workspace.SetWorkingSet(first, initial->fence);
            REQUIRE(opened.HasValue());
            const auto current = opened.Value();
            REQUIRE(current->openPages == std::vector<Assets::AssetId>{Asset(2)});
            REQUIRE(current->document.pages.size() == 2);
            REQUIRE_FALSE(current->dirty);
            REQUIRE(current->fence.generation == initial->fence.generation);
            REQUIRE(current->fence.revision.Value() == initial->fence.revision.Value() + 1);
            REQUIRE(workspace.SetWorkingSet(first, current->fence).Value() == current);
            RequireError(workspace.SetWorkingSet({}, initial->fence), "world_document.stale");
            const std::array missing{Asset(5)};
            RequireError(workspace.SetWorkingSet(missing, current->fence), "world_document.conflict");
            const std::array overCapacity{Asset(2), Asset(4)};
            RequireError(workspace.SetWorkingSet(overCapacity, current->fence), "world_document.capacity");
            REQUIRE(workspace.Snapshot().Value() == current);
            const auto closed = workspace.SetWorkingSet({}, current->fence).Value();
            REQUIRE(closed->openPages.empty());
            REQUIRE(closed->document.pages.size() == 2);
            REQUIRE(initial->openPages.empty());
            REQUIRE(current->openPages.size() == 1);
        }

        TEST_CASE("Partitioned world workspace owns admitted metadata independently of caller storage", "[unit][editor][world_document]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            auto document = Document();
            const auto opened = workspace.Open(document);
            REQUIRE(opened.HasValue());
            const auto snapshot = opened.Value();
            document.layers.front().name = "Changed caller";
            document.pages.clear();
            REQUIRE(snapshot->document.layers.front().name == "Terrain");
            REQUIRE(snapshot->document.pages.size() == 1);
            REQUIRE(snapshot->openPages.empty());
            REQUIRE_FALSE(snapshot->dirty);
            REQUIRE(snapshot->fence.owner == Asset(9));
            REQUIRE(snapshot->fence.generation.IsValid());
            REQUIRE(snapshot->fence.revision.IsValid());
            REQUIRE(workspace.Snapshot().Value() == snapshot);
        }
    }  // namespace
}  // namespace Horo::Editor
