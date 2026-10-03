#include "Horo/Editor/EditorTheme.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
    class ScopedThemeDirectory {
    public:
        ScopedThemeDirectory()
            : path_{std::filesystem::temp_directory_path() /
                    ("horo-theme-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
            std::filesystem::create_directories(path_);
        }

        ~ScopedThemeDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

        void Write(std::string_view name, std::string_view contents) const {
            std::ofstream stream{path_ / name};
            stream << contents;
        }

    private:
        std::filesystem::path path_;
    };
}  // namespace

TEST_CASE("Built-in themes update active style and design tokens", "[unit][editor][gui][theme]") {
    using namespace Horo::Editor;

    Tests::HeadlessEditorGuiFixture imgui;
    Theme::RefreshThemeList();
    REQUIRE(Theme::GetThemeList().size() >= 3);

    Theme::SelectThemeByIndex(0);
    REQUIRE(Theme::GetActiveThemeIndex() == 0);
    const ImVec4 darkSurface = Theme::GetActiveTokens().colors.surfaceRoot;

    Theme::SelectThemeByIndex(1);
    REQUIRE(Theme::GetActiveThemeIndex() == 1);
    REQUIRE(Theme::GetActiveTokens().colors.surfaceRoot.x != darkSurface.x);

    Theme::SelectThemeByIndex(2);
    REQUIRE(Theme::GetActiveThemeIndex() == 2);
    REQUIRE(Theme::GetActiveTokens().colors.textPrimary.x < 0.2F);

    Theme::SelectThemeByIndex(-1);
    Theme::SelectThemeByIndex(1000);
    REQUIRE(Theme::GetActiveThemeIndex() == 2);

    for (const Theme::Preset preset : {Theme::Preset::HoroDark, Theme::Preset::Midnight, Theme::Preset::Light}) {
        Theme::SetThemePreset(preset);
        Theme::ApplyCurrentTheme();
        REQUIRE(Theme::GetThemePreset() == preset);
        const ImVec4 toolbar = Theme::BottomDockToolbarSurface();
        const ImVec4 content = Theme::BottomDockContentSurface();
        REQUIRE((toolbar.x != content.x || toolbar.y != content.y || toolbar.z != content.z));
    }
    Theme::SetUiScalePercent(150);
    REQUIRE(Theme::GetThemePreset() == Theme::Preset::Light);
}

TEST_CASE("Custom theme parsing accepts supported colors and token overrides", "[unit][editor][gui][theme]") {
    using namespace Horo::Editor;

    const auto defaults = DesignSystem::DefaultDesignTokens();
    REQUIRE(defaults.typography.sansBase == 18.0F);
    REQUIRE(defaults.typography.sansCompactBase == 18.0F);
    REQUIRE(defaults.typography.caption == 18.0F);
    REQUIRE(defaults.typography.label == 18.0F);
    REQUIRE(Theme::FontPx::SansCompact == 18.0F);
    REQUIRE(Theme::FontPx::Icon == 20.0F);

    Tests::HeadlessEditorGuiFixture imgui;
    ScopedThemeDirectory directory;
    directory.Write("custom.json", R"({
        "name": "Ocean",
        "colors": {
            "WindowBg": "#102030",
            "ChildBg": [0.1, 0.2, 0.3],
            "Text": [0.9, 0.8, 0.7, 1.0],
            "Ignored": "invalid"
        },
        "tokens": {
            "typography": {
                "sansBase": 16.0,
                "sansCompactBase": 14.0,
                "sansEmphasisBase": 17.0,
                "caption": 9.0,
                "label": 15.0,
                "body": 17.0,
                "cardTitle": 18.0,
                "title": 19.0,
                "heading": 23.0,
                "display": 30.0
            },
            "radii": {"control": 3.0, "card": 7.0, "modal": 11.0},
            "layoutSpacing": {"propertyRowGap": 10.0},
            "styleSpacing": {"xs": 2.0, "s": 4.0, "m": 8.0, "l": 12.0, "xl": 20.0},
            "componentSizes": {"m": {"fontSize": 8.0, "height": 34.0, "paddingX": 10.0, "paddingY": 5.0, "iconSize": 18.0}}
        }
    })");
    directory.Write("invalid.json", "{not-json");

    Theme::ThemeEntry custom;
    REQUIRE(Theme::LoadThemeFromJson((directory.Path() / "custom.json").string().c_str(), custom));
    REQUIRE(custom.name == "Ocean");
    REQUIRE_FALSE(custom.isBuiltIn);
    REQUIRE(custom.colors.contains("WindowBg"));
    REQUIRE(custom.designTokens.typography.sansBase == 16.0F);
    REQUIRE(custom.designTokens.typography.sansCompactBase == 16.0F);
    REQUIRE(custom.designTokens.typography.caption == 16.0F);
    REQUIRE(custom.designTokens.typography.label == 16.0F);
    REQUIRE(custom.designTokens.typography.body == 17.0F);
    REQUIRE(custom.designTokens.typography.cardTitle == 18.0F);
    REQUIRE(custom.designTokens.typography.title == 19.0F);
    REQUIRE(custom.designTokens.typography.heading == 23.0F);
    REQUIRE(custom.designTokens.typography.display == 30.0F);
    REQUIRE(Horo::Editor::DesignSystem::MetricsFor(custom.designTokens, Horo::Editor::DesignSystem::ComponentSize::Medium).fontSize ==
            16.0F);
    REQUIRE(custom.designTokens.radii.modal == 11.0F);
    REQUIRE(custom.designTokens.spacing.propertyRowGap == 10.0F);

    Theme::ThemeEntry invalid;
    REQUIRE_FALSE(Theme::LoadThemeFromJson((directory.Path() / "invalid.json").string().c_str(), invalid));
    REQUIRE_FALSE(Theme::LoadThemeFromJson((directory.Path() / "missing.json").string().c_str(), invalid));

    Theme::RefreshThemeList(directory.Path().string().c_str());
    REQUIRE(Theme::GetThemeList().size() >= 4);
    Theme::SelectThemeByIndex(static_cast<int>(Theme::GetThemeList().size() - 1));
    REQUIRE_FALSE(Theme::GetThemeList().back().isBuiltIn);
    REQUIRE(Theme::GetActiveTokens().typography.sansBase == 16.0F);
}
