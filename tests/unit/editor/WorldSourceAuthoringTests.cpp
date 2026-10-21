#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    namespace WS = WorldStreaming;
    using Point = Math::WorldCoordinate64;

    Assets::AssetId Owner() {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 9;
        return Assets::AssetId::FromBytes(bytes);
    }

    PartitionedWorldDocument Document() {
        return {.settings = {.partition = WS::WorldPartitionId::Create(Owner().Bytes()).Value(),
                             .bounds = {},
                             .grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-1, 1, -1, 1, -1, 1}, 1).Value()}};
    }

    PartitionedWorldSource Source(std::uint64_t id = 1) {
        return {.id = WS::StreamingSourceId::Create(id).Value(),
                .revision = WS::StreamingSourceRevision::Create(1).Value(),
                .shape = WS::StreamingSourceSphere{{}, 1000},
                .priority = WS::StreamingSourcePriority::Create(1).Value()};
    }

    template <typename T> void ErrorIs(const Result<T> &value, const char *code) {
        REQUIRE(value.HasError());
        CHECK(value.ErrorValue().code.Value() == code);
    }

    TEST_CASE("World source edits publish canonical values and undo redo with fresh revisions", "[editor][world_document][source]") {
        PartitionedWorldWorkspace workspace{Owner()};
        auto current = workspace.Open(Document()).Value();
        const auto original = current;
        auto value = Source();
        current = workspace.EditSource({current->fence, value.id, value}).Value();
        const auto created = current;
        REQUIRE(current->document.sources.size() == 1);
        CHECK(original->document.sources.empty());
        value = current->document.sources[0];
        value.target = WS::StreamingDesiredResidency::Activated;
        value.debugColor = {1, 0, 0};
        value.priority = WS::StreamingSourcePriority::Create(7).Value();
        current = workspace.EditSource({current->fence, value.id, value}).Value();
        const auto editedRevision = current->document.sources[0].revision;
        CHECK(current->document.sources[0].target == WS::StreamingDesiredResidency::Activated);
        CHECK(current->dirty);
        current = workspace.ReplaySources(current->fence, false).Value();
        CHECK(current->document.sources[0].priority.Value() == 1);
        CHECK(current->document.sources[0].revision > editedRevision);
        current = workspace.ReplaySources(current->fence, true).Value();
        CHECK(current->document.sources[0].priority.Value() == 7);
        CHECK(current->document.sources[0].target == WS::StreamingDesiredResidency::Activated);
        CHECK(created->document.sources[0].priority.Value() == 1);
        current = workspace.EditSource({current->fence, value.id, {}}).Value();
        CHECK(current->document.sources.empty());
        current = workspace.ReplaySources(current->fence, false).Value();
        REQUIRE(current->document.sources.size() == 1);
        CHECK(current->document.sources[0].id == value.id);
        CHECK(current->document.sources[0].revision > editedRevision);
    }

    TEST_CASE("World source rejected edits preserve source root and history", "[editor][world_document][source]") {
        PartitionedWorldWorkspace workspace{Owner()};
        auto root = workspace.Open(Document()).Value();
        auto source = Source();
        root = workspace.EditSource({root->fence, source.id, source}).Value();
        source = root->document.sources[0];
        std::string_view expected = "world_document.invalid";
        SECTION("foreign value identity") {
            source.id = WS::StreamingSourceId::Create(2).Value();
        }
        SECTION("unsupported state") {
            source.target = static_cast<WS::StreamingDesiredResidency>(99);
            expected = "world_document.unsupported";
        }
        SECTION("unsupported intent") {
            source.intent = static_cast<WS::StreamingSourceIntent>(99);
            expected = "world_document.unsupported";
        }
        SECTION("nonfinite debug color") {
            source.debugColor[0] = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("out of range debug color") {
            source.debugColor[0] = 2;
        }
        SECTION("invalid priority") {
            source.priority = {};
        }
        const auto result = workspace.EditSource({root->fence, WS::StreamingSourceId::Create(1).Value(), source});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == expected);
        CHECK(workspace.Snapshot().Value() == root);
        REQUIRE(workspace.ReplaySources(root->fence, false).HasValue());
    }

    TEST_CASE("Source command no op stale cancellation replacement and shutdown preserve lifecycle fences",
              "[editor][world_document][source]") {
        PartitionedWorldWorkspace workspace{Owner()};
        auto original = workspace.Open(Document()).Value();
        auto source = Source();
        auto root = workspace.EditSource({original->fence, source.id, source}).Value();
        source = root->document.sources[0];
        CHECK(workspace.EditSource({root->fence, source.id, source}).Value() == root);
        ErrorIs(workspace.EditSource({original->fence, source.id, source}), "world_document.stale");
        std::stop_source stop;
        stop.request_stop();
        ErrorIs(workspace.EditSource({root->fence, source.id, {}}, stop.get_token()), "world_document.cancelled");
        ErrorIs(workspace.ReplaySources(root->fence, false, stop.get_token()), "world_document.cancelled");
        CHECK(workspace.Snapshot().Value() == root);
        const auto replacement = workspace.Replace(Document(), root->fence, WorldDocumentDisposition::Discard).Value();
        ErrorIs(workspace.ReplaySources(replacement->fence, false), "world_document.conflict");
        ErrorIs(workspace.EditSource({root->fence, source.id, source}), "world_document.stale");
        REQUIRE(workspace.BeginCancellation().HasValue());
        ErrorIs(workspace.EditSource({replacement->fence, source.id, source}), "world_document.lifecycle");
        REQUIRE(workspace.Shutdown().HasValue());
        CHECK(root->document.sources.size() == 1);
    }

    TEST_CASE("Source ceilings redo branching and generic metadata commits are explicit", "[editor][world_document][source]") {
        PartitionedWorldWorkspace workspace{Owner(), {.maximumSources = 1, .maximumSourceHistory = 2}};
        auto root = workspace.Open(Document()).Value();
        auto source = Source();
        root = workspace.EditSource({root->fence, source.id, source}).Value();
        ErrorIs(workspace.EditSource({root->fence, Source(2).id, Source(2)}), "world_document.capacity");
        source.priority = WS::StreamingSourcePriority::Create(2).Value();
        root = workspace.EditSource({root->fence, source.id, source}).Value();
        ErrorIs(workspace.EditSource({root->fence, source.id, {}}), "world_document.capacity");
        root = workspace.ReplaySources(root->fence, false).Value();
        source.priority = WS::StreamingSourcePriority::Create(3).Value();
        root = workspace.EditSource({root->fence, source.id, source}).Value();
        ErrorIs(workspace.ReplaySources(root->fence, true), "world_document.conflict");
        auto document = root->document;
        document.layers.push_back({.id = WS::StreamingLayerId::Create(1).Value(), .name = "new layer"});
        root = workspace.Commit(document, root->fence).Value();
        ErrorIs(workspace.ReplaySources(root->fence, false), "world_document.conflict");
    }

    TEST_CASE("Imported sources reject duplicates malformed shapes and exhausted revisions", "[editor][world_document][source]") {
        PartitionedWorldWorkspace workspace{Owner()};
        auto document = Document();
        auto source = Source();
        SECTION("duplicate identity") {
            document.sources = {source, source};
            ErrorIs(workspace.Open(document), "world_document.conflict");
        }
        SECTION("malformed sphere") {
            std::get<WS::StreamingSourceSphere>(source.shape).radiusMillimeters = 0;
            document.sources = {source};
            REQUIRE(workspace.Open(document).HasError());
        }
        SECTION("nonwrapping imported revision") {
            source.revision = WS::StreamingSourceRevision::Create(std::numeric_limits<std::uint64_t>::max()).Value();
            document.sources = {source};
            const auto root = workspace.Open(document).Value();
            source.priority = WS::StreamingSourcePriority::Create(2).Value();
            ErrorIs(workspace.EditSource({root->fence, source.id, source}), "world_document.exhausted");
            CHECK(workspace.Snapshot().Value() == root);
        }
    }

    TEST_CASE("Every supported shape has editable canonical viewport control points", "[editor][world_document][gizmo]") {
        auto source = Source();
        std::size_t handle{};
        Point moved = Point::FromMillimeters(100, 0, 0);
        SECTION("sphere center") {}
        SECTION("sphere radius") {
            handle = 1;
            moved = Point::FromMillimeters(1500, 0, 0);
        }
        SECTION("box corner") {
            source.shape = WS::StreamingSourceBox{{}, Point::FromMillimeters(1000, 1000, 1000)};
        }
        SECTION("path vertex") {
            WS::StreamingSourcePathVolume path{.pointCount = 2, .halfExtentMillimeters = 100};
            path.points[1] = Point::FromMillimeters(1000, 0, 0);
            source.shape = path;
            handle = 1;
        }
        SECTION("path half extent") {
            WS::StreamingSourcePathVolume path{.pointCount = 2, .halfExtentMillimeters = 100};
            path.points[1] = Point::FromMillimeters(1000, 0, 0);
            source.shape = path;
            handle = 2;
        }
        SECTION("frustum plane distance") {
            WS::StreamingSourceFrustum frustum{.maximumExtentMillimeters = 1000};
            const std::array<Math::Vec3, 6> normals{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
            for (std::size_t index = 0; index < normals.size(); ++index)
                frustum.planes[index] = {normals[index], 500};
            source.shape = frustum;
            handle = 2;
            moved = Point::FromMillimeters(700, 0, 0);
        }
        const auto points = WorldSourceGizmoPoints(source);
        REQUIRE(points.HasValue());
        const auto result = MoveWorldSourceGizmo(source, handle, moved);
        REQUIRE(result.HasValue());
        CHECK(result.Value().id == source.id);
        CHECK(result.Value().revision == source.revision);
        CHECK(WorldSourceGizmoPoints(result.Value()).Value().Points()[handle] == moved);
        ErrorIs(MoveWorldSourceGizmo(source, points.Value().Points().size(), moved), "world_document.invalid");
    }

    TEST_CASE("Large global origin projection and drag preserve millimeters without signed overflow", "[editor][world_document][gizmo]") {
        const auto low = std::numeric_limits<std::int64_t>::min();
        const auto high = std::numeric_limits<std::int64_t>::max();
        for (const auto origin : {Point::FromMillimeters(high - 10000, low + 10000, 9'000'000'000'000'000),
                                  Point::FromMillimeters(low + 10000, high - 10000, -9'000'000'000'000'000)}) {
            const auto originMm = origin.Millimeters();
            const auto point = Point::FromMillimeters(originMm[0] + 1, originMm[1] - 1, originMm[2] + 1001);
            const auto local = WorldSourceViewportPoint(point, origin);
            REQUIRE(local.HasValue());
            CHECK(std::abs(local.Value().x - 0.001F) < 0.000001F);
            CHECK(WorldSourceViewportGlobal(origin, local.Value()).Value() == point);
            auto source = Source();
            source.shape = WS::StreamingSourceSphere{origin, 1};
            CHECK(WorldSourceGizmoPoints(source).Value().Points()[1].Millimeters()[0] == originMm[0] + 1);
        }
        ErrorIs(WorldSourceViewportPoint(Point::FromMillimeters(high, 0, 0), Point::FromMillimeters(low, 0, 0)),
                "world_document.unsupported");
        ErrorIs(WorldSourceViewportPoint(Point::FromMillimeters(low, 0, 0), Point::FromMillimeters(high, 0, 0)),
                "world_document.unsupported");
        ErrorIs(WorldSourceViewportGlobal(Point::FromMillimeters(high, 0, 0), {0.001F, 0, 0}), "world_document.invalid");
        ErrorIs(WorldSourceViewportGlobal(Point::FromMillimeters(low, 0, 0), {-0.001F, 0, 0}), "world_document.invalid");
        ErrorIs(WorldSourceViewportGlobal({}, {std::numeric_limits<float>::infinity(), 0, 0}), "world_document.unsupported");
    }

    TEST_CASE("World source commands are confined to the canonical owner thread", "[editor][world_document][source]") {
        PartitionedWorldWorkspace workspace{Owner()};
        const auto root = workspace.Open(Document()).Value();
        const auto source = Source();
        std::optional<Result<PartitionedWorldSnapshotLease>> result;
        std::thread worker{[&] {
            result = workspace.EditSource({root->fence, source.id, source});
        }};
        worker.join();
        REQUIRE(result.has_value());
        ErrorIs(*result, "world_document.wrong_thread");
        CHECK(workspace.Snapshot().Value() == root);
    }

    TEST_CASE("Source history preserves the combined canonical page working set and object selection", "[editor][world_document][source]") {
        auto document = Document();
        const auto layer = WS::StreamingLayerId::Create(0).Value();
        const auto revision = WS::WorldAuthoringRevision::Create(1).Value();
        document.layers.push_back({.id = layer, .name = "World"});
        document.pages.push_back(
            {.head = {.partition = document.settings.partition, .sourceAsset = Owner(), .revision = revision},
             .objects = {{.descriptor = {.address = {Owner(), 1}, .revision = revision, .sourceAsset = Owner()}, .layer = layer}}});
        PartitionedWorldWorkspace workspace{Owner()};
        auto root = workspace.Open(document).Value();
        const std::array pages{Owner()};
        root = workspace.SetWorkingSet(pages, root->fence).Value();
        const std::array objects{WS::WorldAuthoringObjectAddress{Owner(), 1}};
        root = workspace.SetSelection(objects, objects[0], root->fence).Value();
        const auto value = Source();
        root = workspace.EditSource({root->fence, value.id, value}).Value();
        CHECK(root->selectedObjects.size() == 1);
        CHECK(root->primary == objects[0]);
        CHECK(root->openPages == std::vector<Assets::AssetId>{Owner()});
        root = workspace.ReplaySources(root->fence, false).Value();
        CHECK(root->document.sources.empty());
        CHECK(root->document.pages == document.pages);
        CHECK(root->selectedObjects.size() == 1);
        CHECK(root->primary == objects[0]);
        CHECK(root->openPages == std::vector<Assets::AssetId>{Owner()});
    }

}  // namespace
