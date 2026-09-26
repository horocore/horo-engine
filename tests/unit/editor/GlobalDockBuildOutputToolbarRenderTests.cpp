#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <imgui.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
    /** @brief Supplies short representative labels so the wide toolbar fits in its dock. */
    class ToolbarLocalization final : public Horo::Editor::ILocalizationService {
    public:
        [[nodiscard]] const std::string &Get(std::string_view, std::string_view key) const override {
            const std::size_t separator = key.rfind('.');
            const std::string label{key.substr(separator == std::string_view::npos ? 0 : separator + 1)};
            return labels_.try_emplace(std::string{key}, label).first->second;
        }

    private:
        mutable std::unordered_map<std::string, std::string> labels_;
    };

    /** @brief Draws one frame of the pane inside a real ImGui window. */
    void DrawToolbarFrame(Horo::Editor::GlobalDockBuildOutputPane &pane, const Horo::Editor::EditorGuiContext &context, const float width,
                          const bool openFilters) {
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({width + 24.0F, 360.0F});
        ImGui::Begin("Build output toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (openFilters)
            ImGui::OpenPopup("##BuildFiltersPopup");
        Horo::Editor::EditorWorkspaceViewCommandData command;
        pane.Draw(ImGui::GetCursorScreenPos(), width, command, context);
        if (openFilters)
            REQUIRE(ImGui::IsPopupOpen("##BuildFiltersPopup"));
        ImGui::End();
    }
}  // namespace

TEST_CASE("Build output toolbar renders extended, compact, and open-filter layouts", "[unit][editor][gui]") {
    using namespace Horo;
    using namespace Horo::Editor;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {2200.0F, 720.0F};
    io.DeltaTime = 1.0F / 60.0F;
    ImFont *font = io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());

    EngineDataBus engineEvents;
    EditorDataBus editorEvents;
    ToolbarLocalization localization;
    const ThemeContext theme{.fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font}};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engineEvents,
                                   .editorEvents = editorEvents,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};

    BuildOutputStore output{8};
    const auto session = output.BeginSession();
    REQUIRE(session.has_value());
    output.Append(BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                    .sessionId = session,
                                    .severity = DiagnosticSeverity::Warning,
                                    .stage = "compile",
                                    .message = "Compiler warning"});
    output.Append(BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                    .sessionId = session,
                                    .severity = DiagnosticSeverity::Error,
                                    .result = BuildOutputResult::Failed,
                                    .stage = "link",
                                    .message = "Link error"});

    GlobalDockBuildOutputPane pane;
    pane.Attach(&output, nullptr, {});
    ImGui::NewFrame();
    DrawToolbarFrame(pane, context, 1800.0F, true);
    ImGui::Render();
    REQUIRE(ImGui::GetDrawData()->CmdListsCount > 0);

    ImGui::NewFrame();
    DrawToolbarFrame(pane, context, 260.0F, false);
    ImGui::Render();
    REQUIRE(ImGui::GetDrawData()->CmdListsCount > 0);
    pane.Detach();
    ImGui::DestroyContext();
}
