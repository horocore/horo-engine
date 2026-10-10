#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    namespace Math = Math;
    using TestWorkspaceController = HoroEditorWorkspaceControllerPolicyTests::FocusedWorkspaceController;
    using namespace HoroEditorWorkspaceControllerFilesystemTests;

    TEST_CASE("Document tabs open, activate and close without losing the center dock", "[unit][editor]") {
        TestWorkspaceController controller;
        EditorWorkspaceViewCommandData open;
        open.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        open.targetIndex = 3;
        open.stringPayload = "horo.game";
        controller.ProcessCommand(open);

        const auto &model = controller.ViewModel();
        const TabStackNode *document = model.workspacePanelHost.Layout().FindTabStack("workspace.document");
        REQUIRE(document != nullptr);
        REQUIRE(document->tabs.size() == 2);
        REQUIRE(model.activeDocumentPanelId == "horo.game");

        EditorWorkspaceViewCommandData close;
        close.command = EditorWorkspaceViewCommand::CloseWorkspacePanel;
        close.stringPayload = "horo.game";
        controller.ProcessCommand(close);
        REQUIRE(document->tabs.size() == 1);
        REQUIRE(model.activeDocumentPanelId == "horo.viewport");

        close.stringPayload = "horo.viewport";
        controller.ProcessCommand(close);
        REQUIRE(document->tabs.empty());
        REQUIRE(model.activeDocumentPanelId.empty());

        open.stringPayload = "horo.viewport";
        controller.ProcessCommand(open);
        REQUIRE(document->tabs.size() == 1);
        REQUIRE(model.activeDocumentPanelId == "horo.viewport");
    }

    TEST_CASE("Docking a side panel into the center updates visible tabs", "[unit][editor]") {
        TestWorkspaceController controller;
        EditorWorkspaceViewCommandData drop;
        drop.command = EditorWorkspaceViewCommand::DockWorkspacePanel;
        drop.stringPayload = "horo.hierarchy";
        drop.workspaceDropTarget = WorkspacePanelDropTarget{"workspace.document", WorkspacePanelHost::DropKind::TabCenter};
        controller.ProcessCommand(drop);

        const auto &model = controller.ViewModel();
        const TabStackNode *document = model.workspacePanelHost.Layout().FindTabStack("workspace.document");
        REQUIRE(document != nullptr);
        REQUIRE(document->tabs.size() == 2);
        REQUIRE(model.activeDocumentPanelId == "horo.hierarchy");
        REQUIRE(model.activeLeftPanelId.empty());

        drop.workspaceDropTarget->kind = WorkspacePanelHost::DropKind::SplitRight;
        controller.ProcessCommand(drop);
        REQUIRE(document->tabs.size() == 2);
        REQUIRE(model.workspacePanelHost.Layout().FindNode("workspace.document.split.horo.hierarchy") == nullptr);

        drop.workspaceDropTarget = WorkspacePanelDropTarget{"workspace.left", WorkspacePanelHost::DropKind::TabCenter};
        controller.ProcessCommand(drop);
        REQUIRE(document->tabs.size() == 1);
        REQUIRE(model.activeDocumentPanelId == "horo.viewport");
        REQUIRE(model.activeLeftPanelId == "horo.hierarchy");

        drop.workspaceDropTarget = WorkspacePanelDropTarget{"workspace.bottom", WorkspacePanelHost::DropKind::SplitRight};
        controller.ProcessCommand(drop);
        REQUIRE(model.activeBottomRightPanelId == "horo.hierarchy");
        REQUIRE(model.activeLeftPanelId.empty());
    }

    TEST_CASE("Moving An Active Panel Across Areas Updates Its Runtime Placement", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 2;
        command.stringPayload = "horo.hierarchy";
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 2, 1};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.activeLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.panelDockAreas.at("horo.hierarchy") == WorkspaceDockArea::Bottom));
        const auto slot = viewModel.activityBarLayout.FindSlot("horo.hierarchy");
        REQUIRE((slot.has_value()));
        REQUIRE((*slot == ActivityBarSlot{ActivityBarRail::Left, 2, 1}));
        REQUIRE((viewModel.activityBarLayout.ItemAt(ActivityBarRail::Left, 2, 0) == "horo.global_dock"));
    }

    TEST_CASE("Replacing A Target Area Preserves The Displaced Panels Placement", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 2;
        command.stringPayload = "horo.hierarchy";
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.activeBottomPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.panelDockAreas.at("horo.global_dock") == WorkspaceDockArea::Bottom));
    }

    TEST_CASE("Dropping Into Bottom Right Splits The Full Bottom Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 2;
        command.stringPayload = "horo.inspector";
        command.bottomDockSlot = BottomDockSlot::Right;
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Split));
        REQUIRE((viewModel.activeBottomPanelId.empty()));
        REQUIRE((viewModel.activeBottomLeftPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeBottomRightPanelId == "horo.inspector"));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Right, 2, 0}));
    }

    TEST_CASE("Clicking A Split Panel Expands It To The Full Bottom Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 2;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.bottomDockSlot = BottomDockSlot::Right;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData clickCommand;
        clickCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        clickCommand.targetIndex = 2;
        clickCommand.stringPayload = "horo.inspector";
        controller.ProcessCommand(clickCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Full));
        REQUIRE((viewModel.activeBottomPanelId == "horo.inspector"));
        REQUIRE((viewModel.activeBottomLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomRightPanelId.empty()));
    }

    TEST_CASE("Dropping A Left Rail Icon Into Bottom Right Moves It To The Right Rail", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData fullCommand;
        fullCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        fullCommand.targetIndex = 2;
        fullCommand.stringPayload = "horo.inspector";
        controller.ProcessCommand(fullCommand);

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 2;
        splitCommand.stringPayload = "horo.hierarchy";
        splitCommand.bottomDockSlot = BottomDockSlot::Right;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(splitCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Split));
        REQUIRE((viewModel.activeBottomLeftPanelId == "horo.inspector"));
        REQUIRE((viewModel.activeBottomRightPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.hierarchy") == ActivityBarSlot{ActivityBarRail::Right, 2, 0}));
    }

    TEST_CASE("Places The Viewport In The Document Top Rail By Default", "[unit][editor]") {
        TestWorkspaceController controller;

        REQUIRE(
            (controller.ViewModel().activityBarLayout.FindSlot("horo.viewport") == ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 0}));
        REQUIRE_FALSE(controller.ViewModel().activityBarLayout.FindSlot("horo.input_mapping").has_value());
    }

    TEST_CASE("Dropping Into The Lower Half Splits The Left Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 0;
        command.stringPayload = "horo.inspector";
        command.sideDockSlot = SideDockSlot::Bottom;
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Split));
        REQUIRE((viewModel.activeLeftPanelId.empty()));
        REQUIRE((viewModel.activeLeftTopPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftBottomPanelId == "horo.inspector"));
        REQUIRE((viewModel.activeRightPanelId.empty()));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Left, 1, 0}));
    }

    TEST_CASE("Dropping Into The Lower Half Splits The Right Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 1;
        command.stringPayload = "horo.hierarchy";
        command.sideDockSlot = SideDockSlot::Bottom;
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 1, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.rightDockMode == SideDockMode::Split));
        REQUIRE((viewModel.activeRightPanelId.empty()));
        REQUIRE((viewModel.activeRightTopPanelId == "horo.inspector"));
        REQUIRE((viewModel.activeRightBottomPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftPanelId.empty()));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.hierarchy") == ActivityBarSlot{ActivityBarRail::Right, 1, 0}));
    }

    TEST_CASE("Clicking A Split Side Panel Expands It To The Full Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 0;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.sideDockSlot = SideDockSlot::Bottom;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData clickCommand;
        clickCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        clickCommand.targetIndex = 0;
        clickCommand.stringPayload = "horo.hierarchy";
        controller.ProcessCommand(clickCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Full));
        REQUIRE((viewModel.activeLeftPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftTopPanelId.empty()));
        REQUIRE((viewModel.activeLeftBottomPanelId.empty()));
    }

    TEST_CASE("Moving One Half Away Expands The Remaining Side Panel", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 0;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.sideDockSlot = SideDockSlot::Bottom;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData moveCommand;
        moveCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        moveCommand.targetIndex = 1;
        moveCommand.stringPayload = "horo.inspector";
        moveCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 0, 0};
        controller.ProcessCommand(moveCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Full));
        REQUIRE((viewModel.activeLeftPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftTopPanelId.empty()));
        REQUIRE((viewModel.activeLeftBottomPanelId.empty()));
    }

    TEST_CASE("Reordering An Active Icon Moves Its Panel And Activates A Source Fallback", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData closeFallback;
        closeFallback.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        closeFallback.targetIndex = 2;
        closeFallback.stringPayload = std::string{};
        controller.ProcessCommand(closeFallback);

        EditorWorkspaceViewCommandData placeFallback;
        placeFallback.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
        placeFallback.stringPayload = "horo.global_dock";
        placeFallback.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 0, 1};
        controller.ProcessCommand(placeFallback);

        EditorWorkspaceViewCommandData openBottom;
        openBottom.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        openBottom.targetIndex = 0;
        openBottom.stringPayload = "horo.inspector";
        openBottom.sideDockSlot = SideDockSlot::Bottom;
        openBottom.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(openBottom);

        EditorWorkspaceViewCommandData moveActiveIcon;
        moveActiveIcon.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
        moveActiveIcon.stringPayload = "horo.hierarchy";
        moveActiveIcon.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 1};
        controller.ProcessCommand(moveActiveIcon);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Split));
        REQUIRE((viewModel.activeLeftTopPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeLeftBottomPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftBottomPanelId != "horo.inspector"));
        REQUIRE((viewModel.panelDockAreas.at("horo.hierarchy") == WorkspaceDockArea::Left));
        REQUIRE((viewModel.panelDockAreas.at("horo.global_dock") == WorkspaceDockArea::Left));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.hierarchy") == ActivityBarSlot{ActivityBarRail::Left, 1, 1}));
    }

    TEST_CASE("Reordering The Only Bottom Icon Does Not Leave A Half Empty Split", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
        command.stringPayload = "horo.global_dock";
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Full));
        REQUIRE((viewModel.activeBottomPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeBottomLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomRightPanelId.empty()));
    }

    TEST_CASE("Dropping On A Side Merge Target Expands The Panel Without Moving Its Icon", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 0;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.sideDockSlot = SideDockSlot::Bottom;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData mergeCommand;
        mergeCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        mergeCommand.targetIndex = 0;
        mergeCommand.stringPayload = "horo.hierarchy";
        controller.ProcessCommand(mergeCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Full));
        REQUIRE((viewModel.activeLeftPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftTopPanelId.empty()));
        REQUIRE((viewModel.activeLeftBottomPanelId.empty()));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.hierarchy") == ActivityBarSlot{ActivityBarRail::Left, 0, 0}));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Left, 1, 0}));

        EditorWorkspaceViewCommandData replaceFullCommand;
        replaceFullCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        replaceFullCommand.targetIndex = 0;
        replaceFullCommand.stringPayload = "horo.inspector";
        controller.ProcessCommand(replaceFullCommand);

        REQUIRE((viewModel.leftDockMode == SideDockMode::Full));
        REQUIRE((viewModel.activeLeftPanelId == "horo.inspector"));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Left, 1, 0}));
    }

    TEST_CASE("Dropping On The Bottom Merge Target Preserves Its Activity Group", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 2;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.bottomDockSlot = BottomDockSlot::Right;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData mergeCommand;
        mergeCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        mergeCommand.targetIndex = 2;
        mergeCommand.stringPayload = "horo.global_dock";
        controller.ProcessCommand(mergeCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Full));
        REQUIRE((viewModel.activeBottomPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeBottomLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomRightPanelId.empty()));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.global_dock") == ActivityBarSlot{ActivityBarRail::Left, 2, 0}));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Right, 2, 0}));
    }

    TEST_CASE("Reordering An Active Bottom Panel To The Left Does Not Render It Twice", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
        command.stringPayload = "horo.global_dock";
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Split));
        REQUIRE((viewModel.activeLeftTopPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftBottomPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeBottomPanelId.empty()));
        REQUIRE((viewModel.activeBottomLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomRightPanelId.empty()));
    }

}  // namespace
