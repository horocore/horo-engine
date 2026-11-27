#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPanel.h"
#include "editor/screens/workspace/panels/global_dock/panes/mcp/GlobalDockMcpPane.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <imgui_internal.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief A host-produced record with an unrelated private field the view must ignore. */
    Log::StructuredLogRecord Audit(std::string event, std::string status, std::string tool, const std::uint64_t operation,
                                   const WorkspaceMcpSessionKey key = {7, 3, 2}) {
        return {.category = "mcp.audit",
                .message = std::move(event),
                .context = "private-ambient-marker",
                .fields = {{"schema_version", std::uint64_t{1}},
                           {"principal_id", key.principal},
                           {"session_id", key.session},
                           {"session_generation", key.generation},
                           {"operation_id", operation},
                           {"approval_id", std::uint64_t{0}},
                           {"elapsed_ms", std::int64_t{12}},
                           {"tool", std::move(tool)},
                           {"status", std::move(status)},
                           {"private_extra", std::string{"private-field-marker"}}}};
    }

    class AuditControls final : public IWorkspaceMcpControls {
    public:
        std::optional<WorkspaceMcpStatus> ReadMcpStatus(const WorkspaceMcpRevision known) const override {
            if (known.session == status.revision.session && known.requests == status.revision.requests)
                return std::nullopt;
            return status;
        }

        Result<void> CancelMcpRequest(const WorkspaceMcpSessionKey session, const std::uint64_t request) override {
            CHECK(session == status.revision.session);
            CHECK(request == 42);
            status.requests.clear();
            ++status.revision.requests;
            return Result<void>::Success();
        }

        Result<void> RevokeMcpSession(const WorkspaceMcpSessionKey session) override {
            CHECK(session == status.revision.session);
            status = {};
            return Result<void>::Success();
        }

        WorkspaceMcpStatus status{{{7, 3, 2}, 1}, {"editor.source.present"}, {{42, "editor.source.open", true}}};
    };

    struct AuditPaneFixture {
        Editor::Tests::EditorGuiContextFixture gui;
        Log::StructuredLogStore log{16};
        GlobalDockMcpPane pane;
        std::string clipboard;

        explicit AuditPaneFixture(IWorkspaceMcpControls *controls = nullptr) {
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.UserData = &clipboard;
            ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext *context, const char *text) {
                *static_cast<std::string *>(context->IO.UserData) = text;
            };
            const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json";
            REQUIRE(gui.localization.LoadCatalogFile(catalog));
            REQUIRE(gui.localization.Prepare(LocaleTag{"en-US"}));
            REQUIRE(gui.localization.ActivatePrepared());
            pane.Attach(&log, controls);
        }

        void Draw() {
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({720, 420});
            ImGui::Begin("MCP dock fixture", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            pane.Draw(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, gui.context);
            ImGui::End();
            gui.imgui.EndFrame();
        }

        void Press(const char *id) {
            auto *window = ImGui::FindWindowByName("MCP dock fixture");
            REQUIRE(window);
            ImGui::FocusWindow(window);
            ImGui::ActivateItemByID(ImHashStr("##chip", 0, window->GetID(id)));
            Draw();
        }

        void PressSessionAction(const char *id) {
            auto *dock = ImGui::FindWindowByName("MCP dock fixture");
            REQUIRE(dock);
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                if (window->ParentWindow != dock || std::string_view{window->Name}.find("##McpRows") == std::string_view::npos)
                    continue;
                ImGui::FocusWindow(window);
                ImGui::ActivateItemByID(window->GetID(id));
                Draw();
                return;
            }
            FAIL("MCP rows child is missing");
        }
    };

    TEST_CASE("MCP dock exports only the visible typed audit history", "[mcp][editor][gui]") {
        AuditPaneFixture fixture;
        fixture.log.Append(Audit("operation.finished", "succeeded", "editor.build.status", 3));
        fixture.log.Append(Audit("operation.finished", "failed", "editor.test.start", 6));
        fixture.log.Append(Audit("operation.finished", "failed", "=private-formula", 12));
        fixture.log.Append(Audit("private-unknown-event", "failed", "editor.source.open", 19));
        fixture.Draw();
        fixture.Press("McpExport");
        CHECK(fixture.clipboard.find("7,3,2,3,0,operation.finished,editor.build.status,succeeded,12") != std::string::npos);
        CHECK(fixture.clipboard.find("editor.test.start") != std::string::npos);
        CHECK(fixture.clipboard.find("private-") == std::string::npos);
        CHECK(fixture.clipboard.find("editor.source.open") == std::string::npos);

        fixture.Press("McpErrors");
        fixture.Press("McpExport");
        CHECK(fixture.clipboard.find("editor.build.status") == std::string::npos);
        CHECK(fixture.clipboard.find("editor.test.start") != std::string::npos);

        fixture.Press("McpPause");
        fixture.log.Append(Audit("operation.finished", "failed", "editor.build.cancel", 22));
        fixture.Press("McpExport");
        CHECK(fixture.clipboard.find("editor.build.cancel") == std::string::npos);
        fixture.Press("McpPause");
        fixture.Press("McpExport");
        CHECK(fixture.clipboard.find("editor.build.cancel") != std::string::npos);

        fixture.pane.Detach();
        fixture.clipboard.clear();
        fixture.Press("McpExport");
        CHECK(fixture.clipboard.empty());
    }

    TEST_CASE("MCP dock controls the exact session while audit history is paused", "[mcp][editor][gui]") {
        AuditControls controls;
        AuditPaneFixture fixture{&controls};
        fixture.Draw();
        fixture.Press("McpPause");
        fixture.PressSessionAction("###McpCancel42");
        CHECK(controls.status.requests.empty());
        fixture.PressSessionAction("###McpRevokeSession");
        CHECK(controls.status.revision.session.principal == 0);
    }

    TEST_CASE("A linked MCP operation resumes history and excludes reused IDs in other sessions", "[mcp][editor][gui]") {
        AuditPaneFixture fixture;
        fixture.log.Append(Audit("operation.finished", "failed", "other.principal", 3, {8, 3, 2}));
        fixture.log.Append(Audit("operation.finished", "failed", "other.session", 3, {7, 4, 2}));
        fixture.log.Append(Audit("operation.finished", "failed", "other.generation", 3, {7, 3, 4}));
        fixture.log.Append(Audit("operation.finished", "failed", "other.operation", 4));
        fixture.Draw();
        fixture.Press("McpErrors");
        fixture.Press("McpPause");
        fixture.pane.FocusAudit({{7, 3, 2}, 3});
        fixture.Draw();
        fixture.Press("McpExport");
        CHECK(fixture.clipboard.find("operation.finished") == std::string::npos);
        fixture.log.Append(Audit("operation.finished", "succeeded", "scene.inspect", 3));
        fixture.Press("McpExport");
        CHECK(fixture.clipboard.find("7,3,2,3,0,operation.finished,scene.inspect,succeeded") != std::string::npos);
        CHECK(fixture.clipboard.find("other.") == std::string::npos);
        fixture.PressSessionAction("###McpClearAudit");
        fixture.Press("McpExport");
        CHECK(fixture.clipboard.find("other.principal") != std::string::npos);
        CHECK(fixture.clipboard.find("scene.inspect") != std::string::npos);
    }

    TEST_CASE("Opening an MCP audit link selects the built-in dock pane", "[mcp][editor][gui]") {
        GlobalDockPanel panel;
        CHECK(panel.ActiveTab() == GlobalDockTab::Assets);
        CHECK_FALSE(panel.FocusMcpAudit({{7, 3, 2}, 0}));
        CHECK(panel.ActiveTab() == GlobalDockTab::Assets);
        REQUIRE(panel.FocusMcpAudit({{7, 3, 2}, 4}));
        CHECK(panel.ActiveTab() == GlobalDockTab::Mcp);
    }
}  // namespace
