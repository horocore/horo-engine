#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    namespace WS = WorldStreaming;
    using Point = Math::WorldCoordinate64;

    Assets::AssetId Owner() {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 17;
        return Assets::AssetId::FromBytes(bytes);
    }

    PartitionedWorldDocument Document() {
        return {.settings = {.partition = WS::WorldPartitionId::Create(Owner().Bytes()).Value(),
                             .bounds = {},
                             .grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-1, 1, -1, 1, -1, 1}, 1).Value()}};
    }

    EditorWorkspaceViewCommandData Create(const WorldViewportBinding &binding) {
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::WorldSourceTool;
        command.worldSource =
            WorldSourceToolCommand{.action = WorldSourceToolAction::Create,
                                   .edit = {.expected = binding.snapshot->fence,
                                            .value = PartitionedWorldSource{.shape = WS::StreamingSourceSphere{binding.globalOrigin, 1000},
                                                                            .priority = WS::StreamingSourcePriority::Create(1).Value()}},
                                   .globalOrigin = binding.globalOrigin,
                                   .viewRevision = binding.viewRevision};
        return command;
    }

    TEST_CASE("Viewport source commands use the composed canonical owner and exact world view", "[editor][world_document][viewport]") {
        PartitionedWorldWorkspace world{Owner()};
        auto root = world.Open(Document()).Value();
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{"test-project", runtime, {}, {.partitionedWorld = &world}};
        const WorldViewportBinding binding{root, Point::FromMillimeters(9'000'000'000'000'000, 0, 0), 1};
        REQUIRE(controller.SetWorldViewportBinding(binding).HasValue());
        controller.ProcessCommand(Create(binding));
        const auto sourceRoot = world.Snapshot().Value();
        REQUIRE(sourceRoot->document.sources.size() == 1);
        CHECK(controller.ViewModel().worldViewport.snapshot == sourceRoot);
        CHECK(std::get<WS::StreamingSourceSphere>(sourceRoot->document.sources[0].shape).center == binding.globalOrigin);
        controller.ProcessCommand(Create(binding));
        REQUIRE(controller.ViewModel().worldSourceError.has_value());
        CHECK(controller.ViewModel().worldSourceError->code.Value() == "world_document.stale");
        CHECK(world.Snapshot().Value() == sourceRoot);
        WorldViewportBinding replacement{sourceRoot, Point::FromMillimeters(9'000'000'000'001'000, 0, 0), 1};
        CHECK(controller.SetWorldViewportBinding(replacement).HasError());
        replacement.viewRevision = 2;
        REQUIRE(controller.SetWorldViewportBinding(replacement).HasValue());
        auto oldView = Create(binding);
        oldView.worldSource->edit.expected = sourceRoot->fence;
        controller.ProcessCommand(oldView);
        CHECK(controller.ViewModel().worldSourceError->code.Value() == "world_document.stale");
        CHECK(world.Snapshot().Value() == sourceRoot);
        auto undo = Create(replacement);
        undo.worldSource->action = WorldSourceToolAction::Undo;
        controller.ProcessCommand(undo);
        CHECK(world.Snapshot().Value()->document.sources.empty());
        CHECK(controller.ViewModel().worldViewport.snapshot == world.Snapshot().Value());
    }

    TEST_CASE("Viewport source binding detaches before the canonical owner is destroyed", "[editor][world_document][viewport]") {
        auto world = std::make_unique<PartitionedWorldWorkspace>(Owner());
        const auto root = world->Open(Document()).Value();
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{"test-project", runtime, {}, {.partitionedWorld = world.get()}};
        const WorldViewportBinding binding{root, {}, 1};
        REQUIRE(controller.SetWorldViewportBinding(binding).HasValue());
        controller.DetachWorldSourceOwner();
        world.reset();
        controller.RefreshWorldSourceProjection();
        controller.ProcessCommand(Create(binding));
        CHECK_FALSE(controller.ViewModel().worldViewport.snapshot);
        CHECK(controller.ViewModel().worldSourceError->code.Value() == "world_document.lifecycle");
        CHECK(root->document.sources.empty());
    }

    TEST_CASE("Actual controller world owner binding rejects implicit replacement and malformed edits",
              "[editor][world_document][viewport]") {
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{"test-project", runtime, {}, {.documentSessionOwner = Owner()}};
        auto &canonical = controller.WorldDocuments();
        auto root = canonical.Open(Document()).Value();
        const WorldViewportBinding binding{root, {}, 1};
        REQUIRE(controller.SetWorldViewportBinding(canonical, binding).HasValue());
        controller.ProcessCommand(Create(binding));
        REQUIRE(controller.HasDirtyWorldDocuments());
        root = canonical.Snapshot().Value();
        auto malformed = Create({root, {}, 1});
        malformed.worldSource->action = WorldSourceToolAction::Edit;
        malformed.worldSource->edit.source = root->document.sources[0].id;
        malformed.worldSource->edit.value.reset();
        controller.ProcessCommand(malformed);
        CHECK(controller.ViewModel().worldSourceError->code.Value() == "world_document.invalid");
        CHECK(canonical.Snapshot().Value() == root);
        const auto replacement = canonical.Replace(Document(), root->fence, WorldDocumentDisposition::Discard).Value();
        controller.RefreshWorldSourceProjection();
        CHECK_FALSE(controller.ViewModel().worldViewport.snapshot);
        CHECK(controller.SetWorldViewportBinding(canonical, {replacement, {}, 1}).HasError());
        controller.ProcessCommand(Create({replacement, {}, 1}));
        CHECK(controller.ViewModel().worldSourceError->code.Value() == "world_document.lifecycle");
        REQUIRE(controller.SetWorldViewportBinding(canonical, {replacement, {}, 2}).HasValue());
        controller.ProcessCommand(Create({replacement, {}, 2}));
        CHECK(canonical.Snapshot().Value()->document.sources.size() == 1);
    }

    TEST_CASE("Source bindings reject missing foreign and closed owners without retargeting", "[editor][world_document][viewport]") {
        PartitionedWorldWorkspace world{Owner()};
        const auto root = world.Open(Document()).Value();
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{"test-project", runtime};
        const WorldViewportBinding binding{root, {}, 1};
        CHECK(controller.SetWorldViewportBinding(binding).HasError());
        REQUIRE(controller.SetWorldViewportBinding(world, binding).HasValue());
        CHECK(controller.SetWorldViewportBinding(world, {{}, {}, 2}).HasError());
        CHECK(controller.SetWorldViewportBinding(world, {root, {}, 0}).HasError());
        auto bytes = Owner().Bytes();
        bytes[0] = 18;
        PartitionedWorldWorkspace foreign{Assets::AssetId::FromBytes(bytes)};
        const auto foreignRoot = foreign.Open(Document()).Value();
        CHECK(controller.SetWorldViewportBinding(foreign, {foreignRoot, {}, 2}).HasError());
        CHECK(controller.ViewModel().worldViewport.snapshot == root);
        REQUIRE(world.Close(root->fence, WorldDocumentDisposition::Discard).HasValue());
        CHECK(controller.SetWorldViewportBinding(world, binding).HasError());
        controller.RefreshWorldSourceProjection();
        CHECK_FALSE(controller.ViewModel().worldViewport.snapshot);
        CHECK(foreign.Snapshot().Value() == foreignRoot);
    }

    TEST_CASE("Source command rejection preserves root when identity space or semantic action is unavailable",
              "[editor][world_document][viewport]") {
        PartitionedWorldWorkspace world{Owner()};
        auto document = Document();
        document.sources.push_back({.id = WS::StreamingSourceId::Create(std::numeric_limits<std::uint64_t>::max()).Value(),
                                    .revision = WS::StreamingSourceRevision::Create(1).Value(),
                                    .shape = WS::StreamingSourceSphere{{}, 1000},
                                    .priority = WS::StreamingSourcePriority::Create(1).Value()});
        const auto root = world.Open(document).Value();
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{"test-project", runtime, {}, {.partitionedWorld = &world}};
        const WorldViewportBinding binding{root, {}, 1};
        REQUIRE(controller.SetWorldViewportBinding(binding).HasValue());
        controller.ProcessCommand(Create(binding));
        REQUIRE(controller.ViewModel().worldSourceError.has_value());
        CHECK(controller.ViewModel().worldSourceError->code.Value() == "world_document.exhausted");
        CHECK(world.Snapshot().Value() == root);
        auto unsupported = Create(binding);
        unsupported.worldSource->action = static_cast<WorldSourceToolAction>(255);
        controller.ProcessCommand(unsupported);
        CHECK(controller.ViewModel().worldSourceError->code.Value() == "world_document.unsupported");
        CHECK(world.Snapshot().Value() == root);
        unsupported.worldSource.reset();
        controller.ProcessCommand(unsupported);
        CHECK(controller.ViewModel().worldSourceError->code.Value() == "world_document.lifecycle");
        CHECK(world.Snapshot().Value() == root);
    }

}  // namespace

TEST_CASE("Registered workspace streaming capture uses its composed bridge and clears on document detach",
          "[editor][viewport][streaming_preview]") {
    using namespace Horo;
    using namespace Horo::Editor;
    PartitionedWorldWorkspace world{Owner()};
    const auto root = world.Open(Document()).Value();
    Runtime::RuntimeSceneService runtime;
    WorldStreamingPreviewHost capture;
    EditorWorkspaceController controller{"test-project", runtime, {}, {.streamingPreview = &capture, .partitionedWorld = &world}};
    REQUIRE(controller.SetWorldViewportBinding({root, {}, 1}).HasValue());
    controller.RefreshWorldDocumentProjection();
    CHECK_FALSE(controller.ViewModel().worldStreamingPreview);
    REQUIRE(controller.ViewModel().worldStreamingPreviewError);
    CHECK(controller.ViewModel().worldStreamingPreviewError->code.Value() == WorldDocumentErrors::Lifecycle.code.Value());
    REQUIRE(world.Close(root->fence).HasValue());
    controller.RefreshWorldDocumentProjection();
    CHECK_FALSE(controller.ViewModel().worldViewport.snapshot);
    CHECK_FALSE(controller.ViewModel().worldStreamingPreview);
    CHECK_FALSE(controller.ViewModel().worldStreamingPreviewError);
    controller.DetachWorldSourceOwner();
    CHECK_FALSE(controller.ViewModel().worldStreamingPreview);
}
