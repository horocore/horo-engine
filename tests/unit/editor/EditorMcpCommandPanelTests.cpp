#include "../../../apps/HoroEditor/app/EditorMcpCommandPanel.h"
#include "../mcp/McpAuthorizationTestSupport.h"
#include "EditorMcpWorkspaceHost.h"
#include "EditorSourceCommandEndpoint.h"
#include "EditorWorkspaceViewModel.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <filesystem>
#include <imgui_internal.h>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct CommandPanelFixture {
        Editor::Tests::EditorGuiContextFixture gui;
        EditorWorkspaceViewModel model;
        EditorWorkspaceViewCommandData command;
        std::vector<std::string> presented;
        std::shared_ptr<const EditorSourceCommandEndpoint> source;
        EditorMcpWorkspaceHost host{std::make_shared<Mcp::Test::Entropy>()};
        std::shared_ptr<IWorkspacePanel> panel{MakeEditorMcpCommandPanel(host)};

        explicit CommandPanelFixture(const std::string &locale) {
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" / (locale + ".json");
            REQUIRE(gui.localization.LoadCatalogFile(catalog));
            REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
            REQUIRE(gui.localization.ActivatePrepared());
            REQUIRE(host.Attach({.projectIdentity = "command-panel-project", .bind = [this](const std::uint64_t revision) {
                source = std::make_shared<EditorSourceCommandEndpoint>(
                    EditorSourceCommandEndpoint{"command-panel-project", revision, std::this_thread::get_id(),
                                                [this](const SourceDocumentId &path, std::optional<EditorSourceCommandPosition>,
                                                       const auto &) {
                    presented.push_back(path.Value());
                    return Result<void>::Success();
                }});
                return EditorWorkspaceCommandEndpoints{.source = source};
            }, .revoke = [this] {
                source.reset();
            }}).HasValue());
            PanelContext context{.dataBus = gui.editorEvents, .inputRouter = &gui.input, .projectIdentity = "command-panel-project"};
            panel->OnAttach(context);
        }

        ~CommandPanelFixture() {
            panel->OnDetach();
        }

        std::string Draw(const bool capture = false) {
            REQUIRE(host.PumpWorkspaceCommands().HasValue());
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({420, 700});
            ImGui::Begin("Command panel fixture", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            if (capture)
                ImGui::LogToBuffer();
            panel->DrawPanel(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail(), model, command, gui.context);
            std::string drawn;
            if (capture) {
                drawn = ImGui::GetCurrentContext()->LogBuffer.c_str();
                ImGui::LogFinish();
            }
            ImGui::End();
            gui.imgui.EndFrame();
            return drawn;
        }

        void Activate(const char *label, const bool text = false) {
            ImGuiWindow *child{};
            for (auto *window : ImGui::GetCurrentContext()->Windows)
                if (window->Active && window->ParentWindow &&
                    std::string_view{window->Name}.find("##McpCommands") != std::string_view::npos)
                    child = window;
            REQUIRE(child);
            ImGui::FocusWindow(child);
            ImGui::ActivateItemByID(child->GetID(label));
            if (text)
                ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
            Draw();
        }
    };

    TEST_CASE("The editor command panel approves only the visible request", "[editor][mcp][gui]") {
        CommandPanelFixture fixture{"en-US"};
        CHECK(fixture.Draw(true).find("Enable source navigation") != std::string::npos);
        CHECK_FALSE(fixture.host.ActiveSession());
        fixture.Activate("###open_source");
        REQUIRE(fixture.host.ActiveSession());
        fixture.Activate("editor.source.open");
        fixture.Activate("##CommandArguments", true);
        ImGui::GetIO().AddInputCharactersUTF8(R"({"path":"gameplay/main.cpp"})");
        fixture.Draw();
        fixture.Activate("###review");
        const auto review = fixture.Draw(true);
        CHECK(review.find("gameplay/main.cpp") != std::string::npos);
        CHECK(fixture.presented.empty());

        fixture.Activate("###deny");
        CHECK(fixture.Draw(true).find("Approval denied.") != std::string::npos);
        CHECK(fixture.presented.empty());
        fixture.Activate("###review");
        fixture.panel->OnPresentationHidden(fixture.command);
        CHECK(fixture.Draw(true).find("Approve once") == std::string::npos);
        CHECK(fixture.presented.empty());

        fixture.Activate("###review");
        fixture.Activate("###approve");
        CHECK(fixture.Draw(true).find("Command response received.") != std::string::npos);
        CHECK(fixture.presented == std::vector<std::string>{"gameplay/main.cpp"});
        fixture.Activate("###close");
        CHECK_FALSE(fixture.host.ActiveSession());
        CHECK_FALSE(fixture.source);
    }

    TEST_CASE("The MCP command panel uses the Turkish catalog", "[editor][mcp][gui]") {
        CommandPanelFixture fixture{"tr-TR"};
        CHECK(fixture.Draw(true).find("CTest komutlarını etkinleştir") != std::string::npos);
        CHECK_FALSE(fixture.host.ActiveSession());
    }
}  // namespace
