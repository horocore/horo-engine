#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "editor/screens/workspace/panels/global_dock/panes/mcp/GlobalDockMcpPane.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <imgui_internal.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief A host-produced record with an unrelated private field the view must ignore. */
    Log::StructuredLogRecord Audit(std::string event, std::string status, std::string tool, const std::uint64_t operation) {
        return {.category = "mcp.audit",
                .message = std::move(event),
                .context = "private-ambient-marker",
                .fields = {{"schema_version", std::uint64_t{1}},
                           {"principal_id", std::uint64_t{7}},
                           {"session_id", std::uint64_t{3}},
                           {"session_generation", std::uint64_t{2}},
                           {"operation_id", operation},
                           {"approval_id", std::uint64_t{0}},
                           {"elapsed_ms", std::int64_t{12}},
                           {"tool", std::move(tool)},
                           {"status", std::move(status)},
                           {"private_extra", std::string{"private-field-marker"}}}};
    }

    struct AuditPaneFixture {
        Editor::Tests::EditorGuiContextFixture gui;
        Log::StructuredLogStore log{16};
        GlobalDockMcpPane pane;
        std::string clipboard;

        AuditPaneFixture() {
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
            pane.Attach(&log);
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
}  // namespace
