#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/hierarchy/HierarchyPanel.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <imgui_internal.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct FocusFixture {
        Editor::Tests::EditorGuiContextFixture gui;
        Input::InputContextToken workspace{
            gui.input.PushContext(Input::InputContextId{"workspace"}, Input::InputContextKind::EditorWorkspace)};
        Input::RawInputCollector collector;
        EditorWorkspaceViewModel model;
        EditorWorkspaceViewCommandData command;

        FocusFixture() {
            ImGui::GetIO().IniFilename = nullptr;
            REQUIRE(
                gui.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json"));
            REQUIRE(gui.localization.Prepare(LocaleTag{"en-US"}));
            REQUIRE(gui.localization.ActivatePrepared());
            model.documentRevision = DocumentRevision{1};
            model.objects.push_back({.id = SceneObjectId{1}, .name = "Object"});
            model.primarySelection = SceneObjectId{1};
            model.selectedObjects = {SceneObjectId{1}};
            Frame(1);
        }

        void Frame(const Input::FrameNumber number) {
            collector.BeginFrame(number);
            gui.input.BeginFrame(collector.Commit());
        }

        void Draw(IWorkspacePanel &panel) {
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({900, 700});
            ImGui::Begin("Focused panel lifecycle", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            panel.DrawPanel(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail(), model, command, gui.context);
            ImGui::End();
            gui.imgui.EndFrame();
        }

        void ActivateField(const bool hierarchy) {
            const auto marker = hierarchy ? "##HierarchyContent" : "##Content";
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                if (!window->Active || !window->ParentWindow || std::string_view{window->ParentWindow->Name} != "Focused panel lifecycle" ||
                    std::string_view{window->Name}.find(marker) == std::string_view::npos)
                    continue;
                auto seed = window->IDStack.back();
                if (!hierarchy)
                    seed = ImHashStr("object_name", 0, seed);
                const auto id = ImHashStr(hierarchy ? "##HierarchySearch" : "##value", 0, seed);
                ImGui::FocusWindow(window);
                ImGui::ActivateItemByID(id);
                ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
                return;
            }
            FAIL("Focused panel content was not drawn");
        }
    };

    TEST_CASE("Hidden text panels release their own focus without revoking another modal", "[editor][input][gui][focus]") {
        const bool hierarchy = GENERATE(true, false);
        const bool withModal = GENERATE(true, false);
        FocusFixture fixture;
        std::unique_ptr<IWorkspacePanel> panel;
        if (hierarchy)
            panel = std::make_unique<HierarchyPanel>();
        else
            panel = std::make_unique<InspectorPanel>();
        PanelContext context{.dataBus = fixture.gui.editorEvents,
                             .inputRouter = &fixture.gui.input,
                             .workspaceInputContext = &fixture.workspace};
        panel->OnAttach(context);
        fixture.Draw(*panel);
        fixture.ActivateField(hierarchy);
        fixture.Draw(*panel);
        REQUIRE_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));
        std::optional<Input::InputContextToken> modal;
        if (withModal)
            modal.emplace(fixture.gui.input.PushContext(Input::InputContextId{"modal"}, Input::InputContextKind::ModalRoot));
        fixture.command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        fixture.command.stringPayload = "preserved";
        panel->OnPresentationHidden(fixture.command);
        CHECK(fixture.command.command == EditorWorkspaceViewCommand::ChangeActivePanel);
        CHECK(fixture.command.stringPayload == "preserved");
        if (modal) {
            CHECK(fixture.gui.input.IsContextActive(*modal));
            CHECK_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));
            modal.reset();
        }
        fixture.Frame(2);
        fixture.Frame(3);
        CHECK(fixture.gui.input.IsContextActive(fixture.workspace));
        panel->OnDetach();
    }
}  // namespace
