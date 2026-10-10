#include "Horo/Editor/EditorUiComponents.h"
#include "SequenceDocumentTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <imgui.h>

TEST_CASE("Shared sequence timeline renders long localized labels at narrow widths in both theme layers",
          "[unit][editor][sequence][render]") {
    using namespace Horo::Editor;
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1200, 900};
    io.DeltaTime = 1.0F / 60.0F;
    auto *font = io.Fonts->AddFontDefault();
    REQUIRE(io.Fonts->Build());
    const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
    Ui::SequenceTimelineLabels labels;
    labels.zoom = labels.scroll = labels.playhead = labels.noContext = labels.unavailable = labels.empty =
        "Uzun yerelleştirilmiş zaman çizelgesi denetim açıklaması";
    labels.trackTypes.fill(labels.zoom);
    const auto source = SequenceDocumentTests::Asset();
    const auto authored = source.Data();
    SequenceTimelineState state;
    for (const auto preset : {Theme::Preset::HoroDark, Theme::Preset::Light}) {
        Theme::SetThemePreset(preset);
        Theme::ApplyCurrentTheme();
        CHECK(ImGui::GetStyle().Colors[ImGuiCol_Text].x == Theme::GetActiveTokens().colors.textPrimary.x);
        for (const float width : {240.0F, 900.0F}) {
            // Two frames resolve child/table sizing before inspecting rendered vertices.
            for (int frame = 0; frame < 2; ++frame) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({width, 800});
                ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoSavedSettings);
                CHECK_FALSE(Ui::SequenceTimeline(source, state, labels, fonts));
                ImGui::End();
                ImGui::Render();
            }
            const auto *draw = ImGui::GetDrawData();
            REQUIRE(draw);
            CHECK(draw->TotalVtxCount > 0);
            bool laneColor = false;
            for (int list = 0; list < draw->CmdListsCount; ++list) {
                for (const auto &vertex : draw->CmdLists[list]->VtxBuffer) {
                    CHECK(std::isfinite(vertex.pos.x));
                    CHECK(std::isfinite(vertex.pos.y));
                    laneColor = laneColor || vertex.col == Theme::U32(Theme::Bg2());
                }
            }
            CHECK(laneColor);
            CHECK(source.Data() == authored);
        }
    }
    Theme::SetThemePreset(Theme::Preset::HoroDark);
    ImGui::DestroyContext();
}
