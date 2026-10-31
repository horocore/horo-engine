#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Foundation/JobSystem.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {
    class ScopedThemeDirectory {
    public:
        ScopedThemeDirectory()
            : path_{std::filesystem::temp_directory_path() /
                    ("horo theme tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
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

TEST_CASE("Theme candidates are fenced to the selected source and bounded before parsing", "[unit][editor][gui][theme][reload]") {
    using namespace Horo::Editor;
    Tests::HeadlessEditorGuiFixture imgui;
    ScopedThemeDirectory directory;
    directory.Write("theme.json", R"({"colors":{"WindowBg":"#112233"}})");
    Theme::RefreshThemeList(directory.Path().string().c_str());
    const int custom = static_cast<int>(Theme::GetThemeList().size() - 1U);
    Theme::ThemeEntry candidate;
    REQUIRE(Theme::LoadThemeFromJson((directory.Path() / "theme.json").string().c_str(), candidate));
    Theme::SelectThemeByIndex(0);
    CHECK_FALSE(Theme::ApplyReloadedTheme(candidate));
    Theme::SelectThemeByIndex(custom);
    REQUIRE(Theme::ApplyReloadedTheme(candidate));
    CHECK(Theme::Bg0().x == 17.0F / 255.0F);
    directory.Write("theme.json", std::string(256U * 1024U + 1U, ' '));
    CHECK_FALSE(Theme::LoadThemeFromJson((directory.Path() / "theme.json").string().c_str(), candidate));
    CHECK(Theme::Bg0().x == 17.0F / 255.0F);
}

TEST_CASE("Theme watcher publishes valid replacements and retains the last good theme after invalid saves",
          "[unit][editor][gui][theme][reload]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Tests::HeadlessEditorGuiFixture imgui;
    ScopedThemeDirectory directory;
    const auto path = directory.Path() / "theme.json";
    directory.Write("theme.json", R"({"colors":{"WindowBg":"#112233"}})");
    Theme::RefreshThemeList(directory.Path().string().c_str());
    Theme::SelectThemeByIndex(static_cast<int>(Theme::GetThemeList().size() - 1U));
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
    EditorDataBus events;
    int reloads{};
    const auto subscription = events.Subscribe<Theme::ThemeReloadedEvent>([&](const auto &) {
        ++reloads;
    });
    Theme::LiveReload watcher{jobs, events};
    const auto pumpUntil = [&](const auto &ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            watcher.Update(0.5);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(ready());
    };
    pumpUntil([&] {
        return reloads == 1;
    });
    directory.Write("theme.json", "{invalid");
    for (int attempt = 0; attempt < 30; ++attempt) {
        watcher.Update(0.5);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    CHECK(reloads == 1);
    CHECK(Theme::Bg0().x == 17.0F / 255.0F);
    directory.Write("theme.json", R"({"colors":{"WindowBg":"#445566"},"tokens":{"radii":{"control":9},"layoutSizes":{"modalWidth":820}}})");
    pumpUntil([&] {
        return reloads == 2;
    });
    CHECK(Theme::Bg0().x == 68.0F / 255.0F);
    CHECK(ImGui::GetStyle().FrameRounding == 9.0F);
    CHECK(Theme::GetActiveTokens().sizes.modalWidth == 820.0F);
    std::filesystem::remove(path);
    for (int attempt = 0; attempt < 20; ++attempt) {
        watcher.Update(0.5);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    CHECK(reloads == 2);
    directory.Write("theme.json", R"({"colors":{"WindowBg":"#778899"}})");
    pumpUntil([&] {
        return reloads == 3;
    });
    CHECK(Theme::Bg0().x == 119.0F / 255.0F);
    Theme::SelectThemeByIndex(0);
    watcher.Update(0.5);
    CHECK(Theme::GetActiveThemeIndex() == 0);
    CHECK(ImGui::GetStyle().FrameRounding == Theme::GetActiveTokens().radii.control);
    watcher.Shutdown();
    watcher.Shutdown();
}

TEST_CASE("Switching to a built-in theme fences a pending background candidate", "[unit][editor][gui][theme][reload]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Tests::HeadlessEditorGuiFixture imgui;
    ScopedThemeDirectory directory;
    directory.Write("theme.json", R"({"colors":{"WindowBg":"#112233"}})");
    Theme::RefreshThemeList(directory.Path().string().c_str());
    Theme::SelectThemeByIndex(static_cast<int>(Theme::GetThemeList().size() - 1U));
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
    EditorDataBus events;
    int reloads{};
    const auto subscription = events.Subscribe<Theme::ThemeReloadedEvent>([&](const auto &) {
        ++reloads;
    });
    Theme::LiveReload watcher{jobs, events};
    watcher.Update(0.5);
    Theme::SelectThemeByIndex(0);
    watcher.Update(0.5);
    watcher.Shutdown();
    CHECK(reloads == 0);
    CHECK(Theme::GetActiveThemeIndex() == 0);
}

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
            "motion": {"focus": 0.75, "reduceMotion": true, "slow": -1},
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
    REQUIRE(custom.designTokens.motion.focus == 0.75F);
    REQUIRE(custom.designTokens.motion.reduceMotion);
    REQUIRE(custom.designTokens.motion.slow == defaults.motion.slow);
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

TEST_CASE("Custom source roles inherit the palette and reject malformed overrides", "[unit][editor][gui][theme][source]") {
    using namespace Horo::Editor;
    Tests::HeadlessEditorGuiFixture imgui;
    ScopedThemeDirectory directory;
    directory.Write("syntax.json", R"({"name":"Source", "tokens":{"sourceSyntax":{
        "text":"#eeeeee", "keyword":"#aabbcc", "type":"#112233", "string":"#ddeeff",
        "number":"#445566", "comment":"#778899", "directive":123, "property":"#12345g"
    }}})");
    Theme::ThemeEntry entry;
    REQUIRE(Theme::LoadThemeFromJson((directory.Path() / "syntax.json").string().c_str(), entry));
    const auto &syntax = entry.designTokens.sourceSyntax;
    REQUIRE(syntax.text.has_value());
    REQUIRE(syntax.keyword.has_value());
    REQUIRE(syntax.type.has_value());
    REQUIRE(syntax.string.has_value());
    REQUIRE(syntax.number.has_value());
    REQUIRE(syntax.comment.has_value());
    REQUIRE_FALSE(syntax.directive.has_value());
    REQUIRE_FALSE(syntax.property.has_value());
    REQUIRE(syntax.keyword->x == 170.0F / 255.0F);
    Theme::RefreshThemeList(directory.Path().string().c_str());
    Theme::SelectThemeByIndex(static_cast<int>(Theme::GetThemeList().size() - 1));
    REQUIRE(Theme::GetActiveTokens().sourceSyntax.keyword.has_value());
    REQUIRE(Theme::GetActiveTokens().sourceSyntax.keyword->x == syntax.keyword->x);
    Theme::SelectThemeByIndex(0);
    REQUIRE_FALSE(Theme::GetActiveTokens().sourceSyntax.keyword.has_value());
}
