#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        Assets::AssetId BrowserAsset(const std::uint8_t value) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return Assets::AssetId::FromBytes(bytes);
        }

        PartitionedWorldDocument BrowserDocument() {
            const auto partition = WS::WorldPartitionId::Create(BrowserAsset(1).Bytes()).Value();
            auto grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-1, 1, -1, 1, -1, 1}, 1).Value();
            PartitionedWorldDocument document{.settings = {.partition = partition,
                                                           .bounds = {Math::WorldCoordinate64::FromMillimeters(-1000, -1000, -1000),
                                                                      Math::WorldCoordinate64::FromMillimeters(1999, 1999, 1999)},
                                                           .grid = grid}};
            document.layers.push_back({.id = WS::StreamingLayerId::Create(0).Value(), .name = "Terrain"});
            document.layers.push_back({.id = WS::StreamingLayerId::Create(1).Value(), .name = "Always"});
            for (std::uint8_t page = 2; page < 4; ++page) {
                const auto revision = WS::WorldAuthoringRevision::Create(1).Value();
                PartitionedWorldPage source{.head = {.partition = partition, .sourceAsset = BrowserAsset(page), .revision = revision}};
                source.objects.push_back({.descriptor = {.address = {.page = BrowserAsset(page), .object = 1},
                                                         .revision = revision,
                                                         .sourceAsset = BrowserAsset(4),
                                                         .bounds = {Math::WorldCoordinate64::FromMillimeters(-1, 0, 0),
                                                                    Math::WorldCoordinate64::FromMillimeters(1000, 0, 0)}},
                                          .layer = document.layers.front().id});
                document.pages.push_back(std::move(source));
            }
            auto always = document.pages.front().objects.front();
            always.descriptor.address.object = 2;
            always.descriptor.placement = WS::WorldSpatialObjectPlacementClass::AlwaysPresent;
            always.layer = document.layers.back().id;
            document.pages.front().objects.push_back(always);
            return document;
        }

        template <typename T> void BrowserError(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == error.code.Value());
        }
    }  // namespace

    TEST_CASE("World browser projects exact canonical source cells and layers without mutating source", "[editor][world_browser]") {
        PartitionedWorldWorkspace workspace{BrowserAsset(9)};
        const auto source = workspace.Open(BrowserDocument()).Value();
        const std::array pages{BrowserAsset(2)};
        const auto open = workspace.SetWorkingSet(pages, source->fence).Value();
        const auto result = workspace.Browse({}, open->fence);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().source == open);
        REQUIRE(result.Value().cells.size() == 3);
        REQUIRE(result.Value().layers.size() == 2);
        CHECK(result.Value().cells.front().id.x == -1);
        CHECK(result.Value().cells.back().id.x == 1);
        for (const auto &cell : result.Value().cells) {
            CHECK(cell.objects == 2);
            CHECK(cell.openObjects == 1);
        }
        CHECK(result.Value().layers.back().alwaysPresentObjects == 1);
        CHECK_FALSE(workspace.Snapshot().Value()->dirty);
        CHECK(workspace.Snapshot().Value() == open);
    }

    TEST_CASE("World browser filters stable identities names coordinates and authoring status", "[editor][world_browser]") {
        PartitionedWorldWorkspace workspace{BrowserAsset(9)};
        auto current = workspace.Open(BrowserDocument()).Value();
        current = workspace.SetWorkingSet(std::array{BrowserAsset(2)}, current->fence).Value();
        WorldBrowserQuery query;
        SECTION("case-insensitive layer search") {
            query.search = "tErRaIn";
        }
        SECTION("explicit stable layer") {
            query.layer = WS::StreamingLayerId::Create(0).Value();
        }
        SECTION("mixed authoring status") {
            query.status = WorldBrowserStatus::Mixed;
        }
        const auto result = workspace.Browse(query, current->fence).Value();
        REQUIRE(result.layers.size() == 1);
        REQUIRE(result.cells.size() == 3);
        query.search = "-1,0,0";
        REQUIRE(workspace.Browse(query, current->fence).Value().cells.size() == 1);
        query.search = "absent";
        REQUIRE(workspace.Browse(query, current->fence).Value().cells.empty());
        REQUIRE(workspace.Browse(query, current->fence).Value().layers.empty());
        query = {.status = WorldBrowserStatus::Open};
        REQUIRE(workspace.Browse(query, current->fence).Value().layers.size() == 1);
        REQUIRE(workspace.Browse(query, current->fence).Value().cells.empty());
    }

    TEST_CASE("World browser honors each object's authored assignment LOD", "[editor][world_browser]") {
        auto document = BrowserDocument();
        document.settings.grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-1, 1, -1, 1, -1, 1}, 2).Value();
        document.pages.back().objects.front().assignmentLod = 1;
        PartitionedWorldWorkspace workspace{BrowserAsset(9)};
        const auto current = workspace.Open(document).Value();
        const auto result = workspace.Browse({}, current->fence).Value();
        REQUIRE(result.cells.size() == 5);
        CHECK(result.cells.front().id.lod == 0);
        CHECK(result.cells.back().id.lod == 1);
    }

    TEST_CASE("Layer rename preserves browser identity while old revision queries remain stale", "[editor][world_browser]") {
        PartitionedWorldWorkspace workspace{BrowserAsset(9)};
        const auto first = workspace.Open(BrowserDocument()).Value();
        auto renamed = first->document;
        const auto identity = renamed.layers.front().id;
        renamed.layers.front().name = "Landscape ü";
        const auto current = workspace.Commit(renamed, first->fence).Value();
        BrowserError(workspace.Browse({}, first->fence), WorldDocumentErrors::Stale);
        const auto byId = workspace.Browse({.layer = identity}, current->fence).Value();
        REQUIRE(byId.layers.size() == 1);
        CHECK(byId.layers.front().descriptor.name == "Landscape ü");
        CHECK(byId.cells.size() == 3);
        CHECK(workspace.Browse({.search = "Terrain"}, current->fence).Value().layers.empty());
        CHECK(workspace.Browse({.search = "ü"}, current->fence).Value().layers.size() == 1);
        CHECK(current->dirty);
        CHECK(workspace.Snapshot().Value() == current);
    }

    TEST_CASE("World browser rejects invalid unsupported cancelled and over-capacity queries atomically", "[editor][world_browser]") {
        PartitionedWorldWorkspace workspace{BrowserAsset(9)};
        const auto current = workspace.Open(BrowserDocument()).Value();
        WorldBrowserQuery query;
        const ErrorCodeDescriptor *error = &WorldDocumentErrors::Capacity;
        SECTION("cell output capacity") {
            query.maximumCells = 2;
        }
        SECTION("zero capacity") {
            query.maximumCells = 0;
        }
        SECTION("excessive ceiling") {
            query.maximumCells = 65537;
        }
        SECTION("search capacity") {
            query.search.assign(257, 'a');
        }
        SECTION("unsupported status") {
            query.status = static_cast<WorldBrowserStatus>(255);
            error = &WorldDocumentErrors::Unsupported;
        }
        SECTION("bad UTF8") {
            query.search = "\xFF";
            error = &WorldDocumentErrors::Invalid;
        }
        SECTION("embedded NUL") {
            query.search = std::string("a\0b", 3);
            error = &WorldDocumentErrors::Invalid;
        }
        SECTION("unknown layer") {
            query.layer = WS::StreamingLayerId::Create(42).Value();
            error = &WorldDocumentErrors::Conflict;
        }
        SECTION("invalid layer") {
            query.layer = WS::StreamingLayerId{};
            error = &WorldDocumentErrors::Invalid;
        }
        BrowserError(workspace.Browse(query, current->fence), *error);
        REQUIRE(workspace.Snapshot().Value() == current);
        std::stop_source stop;
        stop.request_stop();
        BrowserError(workspace.Browse({}, current->fence, stop.get_token()), WorldDocumentErrors::Cancelled);
    }

    TEST_CASE("World browser fences replacement close shutdown and thread ownership while retained results survive",
              "[editor][world_browser]") {
        PartitionedWorldWorkspace workspace{BrowserAsset(9)};
        const auto first = workspace.Open(BrowserDocument()).Value();
        const auto retained = workspace.Browse({}, first->fence).Value();
        const auto replaced = workspace.Replace(BrowserDocument(), first->fence).Value();
        BrowserError(workspace.Browse({}, first->fence), WorldDocumentErrors::Stale);
        std::optional<Result<WorldBrowserSnapshot>> fromWorker;
        std::thread worker([&] {
            fromWorker = workspace.Browse({}, replaced->fence);
        });
        worker.join();
        BrowserError(*fromWorker, WorldDocumentErrors::WrongThread);
        REQUIRE(workspace.BeginCancellation().HasValue());
        REQUIRE(workspace.Browse({}, replaced->fence).HasValue());
        REQUIRE(workspace.Close(replaced->fence).HasValue());
        BrowserError(workspace.Browse({}, replaced->fence), WorldDocumentErrors::Lifecycle);
        REQUIRE(workspace.Shutdown().HasValue());
        BrowserError(workspace.Browse({}, first->fence), WorldDocumentErrors::Lifecycle);
        CHECK(retained.source == first);
        CHECK(retained.cells.size() == 3);
    }
}  // namespace Horo::Editor
