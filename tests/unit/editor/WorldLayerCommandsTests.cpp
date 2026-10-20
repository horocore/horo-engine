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

        TEST_CASE("Owner-issued layer creation uses unused typed IDs and one canonical undo transaction",
                  "[unit][editor][world_document][layers]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            const auto initial = workspace.Open(Document()).Value();
            auto current = workspace.CreateLayer("New layer", initial->fence).Value();
            REQUIRE(current->document.layers.back().id == WS::StreamingLayerId::Create(1).Value());
            REQUIRE(current->document.layers.back().name == "New layer");
            REQUIRE(current->layerUndoCount == 1);
            RequireError(workspace.CreateLayer("Stale", initial->fence), "world_document.stale");
            RequireError(workspace.CreateLayer("", current->fence), "world_document.invalid");
            REQUIRE(workspace.Snapshot().Value() == current);
            current = workspace.ReplayLayerCommand(false, current->fence).Value();
            REQUIRE(current->document.layers == initial->document.layers);
            current = workspace.ReplayLayerCommand(true, current->fence).Value();
            REQUIRE(current->document.layers.back().id == WS::StreamingLayerId::Create(1).Value());
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

        TEST_CASE("Malformed layer filters preserve canonical context and history", "[unit][editor][world_document][layers]") {
            PartitionedWorldWorkspace workspace{Asset(9)};
            const auto initial = workspace.Open(Document()).Value();
            const auto current =
                workspace.ExecuteLayerCommand(SetCurrentWorldLayer{initial->document.layers.front().id}, initial->fence).Value();
            WorldLayerCommand command = FilterWorldLayers{};
            std::string_view code = "world_document.invalid";
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
            RequireError(workspace.ExecuteLayerCommand(command, current->fence), code);
            REQUIRE(workspace.Snapshot().Value() == current);
            REQUIRE(current->layerUndoCount == 1);
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

    }  // namespace
}  // namespace Horo::Editor
