#include "Horo/Editor/DefaultWorkspacePanels.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/accessibility/AccessibilityContrastPanel.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <limits>
#include <utility>

namespace Horo::Editor {
    TEST_CASE("Contrast tool rejects intermediate drafts and never reports a previous valid result", "[editor][accessibility][contrast]") {
        ContrastInspectionDraft draft;
        REQUIRE(draft.Evaluate().Value().ratio == Catch::Approx(21));
        draft.foreground = {'#', '7', '7', '7', '7', '7', '7', '\0'};
        draft.background = {'#', 'F', 'F', 'F', 'F', 'F', 'F', '\0'};
        REQUIRE_FALSE(draft.Evaluate().Value().meetsAA);
        draft.size = Accessibility::ContrastTextSize::Large;
        REQUIRE(draft.Evaluate().Value().meetsAA);
        draft.foreground[3] = '\0';
        REQUIRE(draft.Evaluate().HasError());
        draft.foreground[3] = 'g';
        REQUIRE(draft.Evaluate().HasError());
        draft.foreground[3] = '7';
        draft.foreground.back() = '7';
        REQUIRE(draft.Evaluate().HasError());
    }

    TEST_CASE("Theme contrast inspection preserves full token precision and rejects unknown composition",
              "[editor][accessibility][contrast]") {
        const ImVec4 text{0.465F, 0.465F, 0.465F, 1};
        const ImVec4 background{1, 1, 1, 1};
        const auto exact = Accessibility::EvaluateTextContrast({text.x, text.y, text.z}, {1, 1, 1}).Value();
        const auto actual = EvaluateOpaqueContrastPair(text, background, Accessibility::ContrastTextSize::Standard).Value();
        REQUIRE(actual.ratio == exact.ratio);
        REQUIRE(actual.meetsAA == exact.meetsAA);
        auto translucent = text;
        translucent.w = 0.5F;
        REQUIRE(EvaluateOpaqueContrastPair(translucent, background, Accessibility::ContrastTextSize::Standard).HasError());
        translucent = background;
        translucent.w = 0;
        REQUIRE(EvaluateOpaqueContrastPair(text, translucent, Accessibility::ContrastTextSize::Standard).HasError());
        auto invalid = text;
        invalid.x = std::numeric_limits<float>::quiet_NaN();
        REQUIRE(EvaluateOpaqueContrastPair(invalid, background, Accessibility::ContrastTextSize::Standard).HasError());
    }

    TEST_CASE("Theme contrast selections read both active layers and reject unknown or uncomposed pairs",
              "[editor][accessibility][contrast][headless][theme]") {
        Tests::HeadlessEditorGuiFixture fixture;
        const auto category = Accessibility::ContrastTextSize::Standard;
        const std::array pairs{std::pair{Theme::Text(), Theme::Bg0()},
                               std::pair{Theme::Text(), Theme::Bg1()},
                               std::pair{Theme::Text(), Theme::Bg2()},
                               std::pair{Theme::Text(), Theme::Bg3()},
                               std::pair{Theme::Muted(), Theme::Bg0()},
                               std::pair{Theme::Muted(), Theme::Bg2()},
                               std::pair{Theme::Dim(), Theme::Bg3()},
                               std::pair{ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(ImGuiCol_WindowBg)},
                               std::pair{ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(ImGuiCol_PopupBg)},
                               std::pair{ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)},
                               std::pair{ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), ImGui::GetStyleColorVec4(ImGuiCol_WindowBg)}};
        static_assert(pairs.size() == static_cast<std::size_t>(ThemeContrastPair::Count));
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            INFO(index);
            const auto expected = EvaluateOpaqueContrastPair(pairs[index].first, pairs[index].second, category);
            const auto actual = EvaluateThemeContrastPair(static_cast<ThemeContrastPair>(index), category);
            REQUIRE(actual.HasValue() == expected.HasValue());
            if (expected.HasValue()) {
                CHECK(actual.Value().ratio == expected.Value().ratio);
                CHECK(actual.Value().meetsAA == expected.Value().meetsAA);
                CHECK(actual.Value().meetsAAA == expected.Value().meetsAAA);
            }
        }
        REQUIRE(EvaluateThemeContrastPair(ThemeContrastPair::Count, category).HasError());
        REQUIRE(EvaluateThemeContrastPair(static_cast<ThemeContrastPair>(255), category).HasError());
        auto &colors = ImGui::GetStyle().Colors;
        colors[ImGuiCol_Text] = {0, 0, 0, 1};
        colors[ImGuiCol_WindowBg] = {1, 1, 1, 1};
        REQUIRE(EvaluateThemeContrastPair(ThemeContrastPair::NativeTextWindow, category).Value().ratio == Catch::Approx(21));
        colors[ImGuiCol_WindowBg] = {0, 0, 0, 1};
        REQUIRE(EvaluateThemeContrastPair(ThemeContrastPair::NativeTextWindow, category).Value().ratio == Catch::Approx(1));
        colors[ImGuiCol_WindowBg].w = 0.5F;
        REQUIRE(EvaluateThemeContrastPair(ThemeContrastPair::NativeTextWindow, category).HasError());
    }

    TEST_CASE("Contrast workspace tool renders paired locales at narrow and wide bounds without a display",
              "[editor][accessibility][contrast][headless]") {
        Tests::EditorGuiContextFixture fixture;
        const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        for (const char *locale : {"en-US", "tr-TR"})
            REQUIRE(fixture.localization.LoadCatalogFile(root / (std::string{locale} + ".json")));
        WorkspacePanelRegistry registry;
        RegisterDefaultWorkspacePanels(registry);
        const auto &panels = registry.GetAllPanels();
        const auto panel = std::ranges::find_if(panels, [](const auto &entry) {
            return entry->GetId() == "horo.accessibility_contrast";
        });
        REQUIRE(panel != panels.end());
        EditorWorkspaceViewModel viewModel;
        EditorWorkspaceViewCommandData command;
        for (const char *locale : {"en-US", "tr-TR"}) {
            REQUIRE(fixture.localization.Prepare({locale}));
            REQUIRE(fixture.localization.ActivatePrepared());
            for (const float width : {220.0F, 640.0F}) {
                fixture.imgui.BeginFrame();
                ImGui::SetNextWindowSize({width, 700}, ImGuiCond_Always);
                ImGui::Begin("Contrast tool test");
                (*panel)->DrawPanel({}, ImGui::GetContentRegionAvail(), viewModel, command, fixture.context);
                ImGui::End();
                fixture.imgui.EndFrame();
                REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
            }
        }
    }

    TEST_CASE("Built-in primary text on opaque editor panels meets standard AA in both theme layers",
              "[editor][accessibility][contrast][headless][theme]") {
        Tests::HeadlessEditorGuiFixture fixture;
        Theme::RefreshThemeList();

        struct RestoreTheme {
            ~RestoreTheme() {
                Theme::SelectThemeByIndex(0);
            }
        } restore;

        std::size_t checked = 0;
        const auto &themes = Theme::GetThemeList();
        for (std::size_t index = 0; index < themes.size(); ++index) {
            if (!themes[index].isBuiltIn)
                continue;
            INFO(themes[index].name);
            Theme::SelectThemeByIndex(static_cast<int>(index));
            for (const ImVec4 background : {Theme::Bg0(), Theme::Bg1(), Theme::Bg2(), Theme::Bg3()}) {
                const auto result = EvaluateOpaqueContrastPair(Theme::Text(), background, Accessibility::ContrastTextSize::Standard);
                REQUIRE(result.HasValue());
                CHECK(result.Value().meetsAA);
            }
            const auto native =
                EvaluateOpaqueContrastPair(ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(ImGuiCol_WindowBg),
                                           Accessibility::ContrastTextSize::Standard);
            REQUIRE(native.HasValue());
            CHECK(native.Value().meetsAA);
            ++checked;
        }
        REQUIRE(checked >= 3);
    }
}  // namespace Horo::Editor
