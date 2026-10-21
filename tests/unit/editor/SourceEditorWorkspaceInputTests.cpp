#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "Horo/Editor/GuiScreen.h"
#include "editor/input/EditorInputActions.h"
#include "editor/screens/workspace/EditorWorkspaceInputRouting.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "support/OwnedTestDirectory.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>

TEST_CASE("Actual workspace carries first-frame source ownership when Delete and navigation produce no edit",
          "[editor][source][widget][input]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Editor::Tests::EditorGuiContextFixture gui;
    Horo::Tests::OwnedTestDirectory directory{"source-first-focus"};
    std::ofstream(directory.Path() / "empty.cpp");
    HoroEditorWorkspaceControllerPolicyTests::FocusedWorkspaceController controller{directory.Path(), {}, [](const SourceOpenResult &) {
        return true;
    }};
    EditorWorkspaceViewCommandData open;
    open.command = EditorWorkspaceViewCommand::OpenSourceFile;
    open.sourceOpenRequest = SourceOpenRequest{.path = "empty.cpp", .mode = SourceOpenMode::EmbeddedOnly};
    controller.ProcessCommand(open);
    REQUIRE(controller.ViewModel().sourceDocuments.size() == 1);
    WorkspacePanelRegistry panels;
    REQUIRE(gui.input.SetActionMap(BuildEditorInputActions()).HasValue());
    auto inputContext =
        gui.input.PushContext(Input::InputContextId{kEditorWorkspaceInputContext}, Input::InputContextKind::EditorWorkspace);
    Input::RawInputCollector collector;
    collector.BeginFrame(1);
    collector.SetKey(Input::Key::Delete, true);
    gui.input.BeginFrame(collector.Commit());
    EditorWorkspaceView view{gui.context, panels, 0, gui.input, inputContext};
    ImGui::GetIO().WantTextInput = false;
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true);
    gui.imgui.BeginFrame();
    EditorWorkspaceViewCommandData command;
    view.Draw(controller.ViewModel(), command, {.width = 1280, .height = 800});
    RouteWorkspaceInputAction(gui.input, inputContext, controller.ViewModel(), command, {});
    gui.imgui.EndFrame();
    CHECK(command.command == EditorWorkspaceViewCommand::None);
    CHECK(command.sourceTextInputOwned);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, true);
    gui.imgui.BeginFrame();
    command = {};
    view.Draw(controller.ViewModel(), command, {.width = 1280, .height = 800});
    gui.imgui.EndFrame();
    CHECK(command.command == EditorWorkspaceViewCommand::None);
    CHECK(command.sourceTextInputOwned);
}

TEST_CASE("Actual workspace shortcut router suppresses scene edits for source ownership and preserves Save",
          "[editor][source][widget][input]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Input::InputRouter input;
    REQUIRE(input.SetActionMap(BuildEditorInputActions()).HasValue());
    auto context = input.PushContext(Input::InputContextId{kEditorWorkspaceInputContext}, Input::InputContextKind::EditorWorkspace);
    Input::RawInputCollector collector;
    EditorWorkspaceViewModel model;
    model.primarySelection = SceneObjectId{7};
    const auto key = GENERATE(Input::Key::Delete, Input::Key::Z, Input::Key::W);
    collector.BeginFrame(1);
    collector.SetModifiers({.control = key == Input::Key::Z});
    collector.SetKey(key, true);
    input.BeginFrame(collector.Commit());
    EditorWorkspaceViewCommandData captured;
    const auto capture = GENERATE(0, 1, 2);
    captured.sourceTextInputOwned = capture == 0;
    RouteWorkspaceInputAction(input, context, model, captured, {.textInputRequested = capture == 1, .popupOpen = capture == 2});
    CHECK(captured.command == EditorWorkspaceViewCommand::None);
    collector.BeginFrame(2);
    collector.SetKey(key, false);
    input.BeginFrame(collector.Commit());
    collector.BeginFrame(3);
    collector.SetKey(key, true);
    input.BeginFrame(collector.Commit());
    EditorWorkspaceViewCommandData scene;
    RouteWorkspaceInputAction(input, context, model, scene, {});
    CHECK(scene.command != EditorWorkspaceViewCommand::None);
    collector.BeginFrame(4);
    collector.SetKey(key, false);
    collector.SetModifiers({.control = true});
    collector.SetKey(Input::Key::S, true);
    input.BeginFrame(collector.Commit());
    EditorWorkspaceViewCommandData save;
    save.sourceTextInputOwned = true;
    RouteWorkspaceInputAction(input, context, model, save, {.textInputRequested = true, .popupOpen = true});
    CHECK(save.command == EditorWorkspaceViewCommand::SaveScene);
}

TEST_CASE("Application Save remains source-document scoped while first-frame source text owns input", "[editor][source][widget][input]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Horo::Tests::OwnedTestDirectory directory{"source-save-input"};
    std::ofstream(directory.Path() / "code.cpp") << "saved";
    HoroEditorWorkspaceControllerPolicyTests::FocusedWorkspaceController controller{directory.Path(), {}, [](const SourceOpenResult &) {
        return true;
    }};
    EditorWorkspaceViewCommandData open;
    open.command = EditorWorkspaceViewCommand::OpenSourceFile;
    open.sourceOpenRequest = SourceOpenRequest{.path = "code.cpp", .mode = SourceOpenMode::EmbeddedOnly};
    controller.ProcessCommand(open);
    REQUIRE(controller.ViewModel().sourceDocuments.size() == 1);
    Input::InputRouter input;
    REQUIRE(input.SetActionMap(BuildEditorInputActions()).HasValue());
    auto context = input.PushContext(Input::InputContextId{kEditorWorkspaceInputContext}, Input::InputContextKind::EditorWorkspace);
    Input::RawInputCollector collector;
    collector.BeginFrame(1);
    collector.SetModifiers({.control = true});
    collector.SetKey(Input::Key::S, true);
    input.BeginFrame(collector.Commit());
    EditorWorkspaceViewCommandData command;
    command.sourceTextInputOwned = true;
    RouteWorkspaceInputAction(input, context, controller.ViewModel(), command, {.textInputRequested = true});
    CHECK(command.command == EditorWorkspaceViewCommand::SaveSourceDocument);
    REQUIRE(command.sourceSave);
    CHECK(command.sourceSave->instance == controller.ViewModel().sourceDocuments.front().Identity().instance);
    controller.ProcessCommand(command);
    REQUIRE(controller.SourceSaveOutcome());
    CHECK(controller.SourceSaveOutcome()->HasValue());
}
