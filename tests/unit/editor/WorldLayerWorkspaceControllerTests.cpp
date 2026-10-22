#include "OwnedTestDirectory.h"
#include "PartitionedWorldResidencyTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/WorldLayerOutliner.h"

namespace Horo::Editor::WorldResidencyTests {
    TEST_CASE("Layer outliner routes through actual editor controller and preserves canonical page selection and payloads",
              "[editor][layers][controller]") {
        Horo::Tests::OwnedTestDirectory project{"layer authoring ü space"};
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{project.Path(), runtime, {}, EditorWorkspaceDependencies{.documentSessionOwner = Asset(9)}};
        auto &world = controller.WorldDocuments();
        const auto document = Document();
        REQUIRE(world.Open(document).HasValue());
        REQUIRE(world.LoadPage(document.pages.front().head, Source(), Current(world)->fence).HasValue());
        REQUIRE(world.SetPagePinned(Asset(2), true, Current(world)->fence).HasValue());
        const std::array selection{document.pages.front().objects.front().descriptor.address};
        REQUIRE(controller.SelectWorldObjects(selection, selection.front(), Current(world)->fence).HasValue());
        const auto before = Current(world);
        REQUIRE(world.CreateLayer("New layer", before->fence).HasValue());
        auto current = Current(world);
        const auto created = current->document.layers.back().id;
        EditorWorkspaceViewCommandData command;
        RouteWorldLayerOutlinerIntent({.action = WorldLayerOutlinerAction::Execute,
                                       .expected = current->fence,
                                       .command = SetCurrentWorldLayer{created}},
                                      command);
        REQUIRE(command.command == EditorWorkspaceViewCommand::WorldLayerAuthoring);
        controller.ProcessCommand(command);
        REQUIRE_FALSE(controller.ViewModel().worldLayerFailure.has_value());
        current = Current(world);
        CHECK(current->currentLayer == created);
        CHECK(current->selectedObjects == before->selectedObjects);
        CHECK(current->primary == before->primary);
        REQUIRE(current->loadedPages.size() == 1);
        CHECK(current->loadedPages.front().pinned);
        CHECK(current->loadedPages.front().content.Objects().front().name == "Box");
        const auto retained = current;
        controller.ProcessCommand(command);
        REQUIRE(controller.ViewModel().worldLayerFailure.has_value());
        CHECK(controller.ViewModel().worldLayerFailure->code.Value() == WorldDocumentErrors::Stale.code.Value());
        CHECK(Current(world) == retained);
        RouteWorldLayerOutlinerIntent({.action = WorldLayerOutlinerAction::Undo, .expected = current->fence}, command);
        controller.ProcessCommand(command);
        CHECK_FALSE(Current(world)->currentLayer.has_value());
        CHECK(Current(world)->selectedObjects == before->selectedObjects);
        CHECK(Current(world)->loadedPages.front().pinned);
    }

    TEST_CASE("Real loaded-page edits cannot be cleared by an earlier layer undo transaction", "[editor][layers][world_residency]") {
        PartitionedWorldWorkspace world{Asset(9)};
        const auto document = Document();
        REQUIRE(world.Open(document).HasValue());
        REQUIRE(world.LoadPage(document.pages.front().head, Source(), Current(world)->fence).HasValue());
        REQUIRE(world.ExecuteLayerCommand(FilterWorldLayers{{"Terrain", {}}}, Current(world)->fence).HasValue());
        CHECK(Current(world)->layerUndoCount == 1);
        REQUIRE(world.CommitLoadedPage(Next(document.pages.front()), Source("Edited"), Current(world)->fence).HasValue());
        const auto current = Current(world);
        CHECK(current->dirty);
        CHECK(current->layerUndoCount == 0);
        ErrorIs(world.ReplayLayerCommand(false, current->fence), "world_document.conflict");
        CHECK(Current(world) == current);
        CHECK(current->loadedPages.front().content.IsDirty());
        CHECK(current->loadedPages.front().content.Objects().front().name == "Edited");
    }

    TEST_CASE("Layer outliner creation and empty intents use explicit screen and controller routes", "[editor][layers][controller]") {
        EditorWorkspaceViewCommandData command;
        RouteWorldLayerOutlinerIntent({}, command);
        CHECK(command.command == EditorWorkspaceViewCommand::None);
        WorldDocumentFence expected{.owner = Asset(9)};
        RouteWorldLayerOutlinerIntent({.action = WorldLayerOutlinerAction::Create, .expected = expected}, command);
        CHECK(command.command == EditorWorkspaceViewCommand::OpenCreateWorldLayer);
        REQUIRE(command.worldLayerIntent.has_value());
        CHECK(command.worldLayerIntent->expected == expected);
    }
}  // namespace Horo::Editor::WorldResidencyTests
