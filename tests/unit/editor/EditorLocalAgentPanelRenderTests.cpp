#include "../../../apps/HoroEditor/app/EditorLocalAgentPanel.h"
#include "../../support/OwnedTestDirectory.h"
#include "EditorWorkspaceViewModel.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/Platform.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <imgui_internal.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct LocalAgentFixture final {
        Editor::Tests::EditorGuiContextFixture gui;
        Editor::Tests::ScopedJobSystem jobs;
        NativeDurableFileSystem files;
        WorkspacePanelRegistry panels;
        Input::InputContextToken workspace{
            gui.input.PushContext(Input::InputContextId{"workspace"}, Input::InputContextKind::EditorWorkspace)};
        Input::RawInputCollector collector;
        EditorWorkspaceViewModel model;
        EditorWorkspaceViewCommandData command;
        EditorLocalAgentPanelHost agent{MakeEditorLocalAgentPanel(jobs.Get(), files)};

        explicit LocalAgentFixture(const std::string locale) {
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" / (locale + ".json");
            REQUIRE(gui.localization.LoadCatalogFile(catalog));
            REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
            REQUIRE(gui.localization.ActivatePrepared());
            panels.RegisterPanel(agent.panel);
            PanelContext ctx{.dataBus = gui.editorEvents,
                             .inputRouter = &gui.input,
                             .workspaceInputContext = &workspace,
                             .projectIdentity = "layout-test"};
            panels.AttachAll(ctx);
            InputFrame(1);
        }

        ~LocalAgentFixture() {
            panels.DetachAll();
            Theme::ApplyUiScale(100, 1.0F);
        }

        void InputFrame(const Input::FrameNumber number) {
            collector.BeginFrame(number);
            gui.input.BeginFrame(collector.Commit());
        }

        void Draw(const float width, const bool show = true) {
            agent.poll();
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({width, 750});
            ImGui::Begin("Chat dock fixture", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            if (show)
                agent.panel->DrawPanel(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail(), model, command, gui.context);
            ImGui::End();
            gui.imgui.EndFrame();
        }

        ImGuiWindow *ChatWindow() const {
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                if (window->Active && window->ParentWindow && std::string_view{window->Name}.find("##LocalAgent") != std::string_view::npos)
                    return window;
            }
            return nullptr;
        }
    };

    TEST_CASE("Local chat owns keyboard input only while its text field is presented", "[editor][agent][gui]") {
        LocalAgentFixture fixture{"en-US"};
        fixture.Draw(420);
        auto *chat = fixture.ChatWindow();
        REQUIRE(chat);
        ImGui::FocusWindow(chat);
        ImGui::ActivateItemByID(chat->GetID("##Prompt"));
        ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
        fixture.Draw(420);
        REQUIRE_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));

        auto modal = fixture.gui.input.PushContext(Input::InputContextId{"modal"}, Input::InputContextKind::ModalRoot);
        fixture.command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        fixture.agent.panel->OnPresentationHidden(fixture.command);
        CHECK(fixture.command.command == EditorWorkspaceViewCommand::ChangeActivePanel);
        CHECK(fixture.gui.input.IsContextActive(modal));
        modal.Reset();
        fixture.InputFrame(2);
        fixture.Draw(420, false);
        fixture.InputFrame(3);
        fixture.Draw(420, false);
        CHECK(fixture.gui.input.IsContextActive(fixture.workspace));
        CHECK_FALSE(fixture.ChatWindow());
        fixture.Draw(420);
        CHECK(fixture.ChatWindow() == chat);
    }

    TEST_CASE("Local chat can move, close and restore as a workspace dock tab", "[editor][agent][gui]") {
        LocalAgentFixture fixture{"en-US"};
        auto &host = fixture.model.workspacePanelHost;
        const auto id = fixture.agent.panel->GetId();
        REQUIRE(host.OpenPanel(id, "workspace.right").Succeeded());
        REQUIRE(host.MovePanel(id, {"workspace.left", 0}).Succeeded());
        CHECK(host.Layout().FindTabStack("workspace.left")->activeTab == id);
        REQUIRE(host.ClosePanel(id).Succeeded());
        REQUIRE(host.OpenPanel(id, "workspace.right").Succeeded());

        const Horo::Tests::OwnedTestDirectory directory{"local-chat-layout"};
        const auto path = directory.Path() / "layout.json";
        AtomicFileReplacementReceipt receipt;
        REQUIRE(host.SaveLayout(fixture.files, receipt, path));
        WorkspacePanelHost restored;
        REQUIRE(restored.RestoreLayout(path));
        CHECK(restored.Layout().FindTabStack("workspace.right")->activeTab == id);
        CHECK(restored.Layout().FindTabStack("workspace.document")->activeTab == "horo.viewport");
        CHECK(restored.DocumentTabs().empty());
    }

    TEST_CASE("Local chat stays in the right dock and keeps localized controls within its resized width", "[editor][agent][gui]") {
        const std::string locale = GENERATE("en-US", "tr-TR");
        const float displayScale = GENERATE(1.0F, 1.5F, 2.0F);
        LocalAgentFixture fixture{locale};
        Theme::ApplyUiScale(100, displayScale);
        ImGui::GetIO().DisplayFramebufferScale = {displayScale, displayScale};
        const auto &right = fixture.panels.GetPanelsForArea(WorkspaceDockArea::Right);
        REQUIRE(right.size() == 1);
        CHECK(right.front()->GetId() == "horo.local_agent");
        for (const auto width : {430.0F, 185.0F, 430.0F}) {
            CAPTURE(locale, displayScale, width);
            // ContentSizeIdeal is from the preceding frame. Allow the vertical scrollbar allocation to settle first.
            fixture.Draw(width);
            fixture.Draw(width);
            fixture.Draw(width);
            const auto *chat = fixture.ChatWindow();
            REQUIRE(chat);
            CHECK(chat->ContentSizeIdeal.x <= chat->InnerRect.GetWidth());
            CHECK(chat->Size.x < width);
        }
    }
}  // namespace
