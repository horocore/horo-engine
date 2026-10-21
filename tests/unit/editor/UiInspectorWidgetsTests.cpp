#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/UiCanvasInspectorPanel.h"
#include "editor/screens/workspace/panels/inspector/UiInspectorAuthoringWidgets.h"
#include "editor/screens/workspace/panels/inspector/UiInspectorWidgets.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>
#include <unordered_map>

using namespace Horo::Editor;
using namespace Horo::Runtime::Ui;

TEST_CASE("UI inspector layout capture and cancel preserve exact signed fixed-point values", "[editor][ui][inspector]") {
    UiLayoutStyle value;
    value.width = UiLength::Dip(std::numeric_limits<std::int32_t>::max());
    value.margin.left = std::numeric_limits<std::int32_t>::min();
    UiInspectorWidgets::Buffers buffers;
    UiInspectorWidgets::CaptureLayout(value, buffers);
    REQUIRE(buffers.IsValid());
    std::int32_t parsed{};
    REQUIRE(buffers.integers[0].Parse(parsed));
    CHECK(parsed == std::numeric_limits<std::int32_t>::max());
    REQUIRE(buffers.integers[6].Parse(parsed));
    CHECK(parsed == std::numeric_limits<std::int32_t>::min());
    buffers.integers[0].valid = false;
    CHECK_FALSE(buffers.IsValid());
    UiInspectorWidgets::CaptureLayout(value, buffers);
    CHECK(buffers.IsValid());
}

TEST_CASE("UI inspector style capture preserves unsigned flags and resets invalid text on source changes", "[editor][ui][inspector]") {
    UiInspectorWidgets::Buffers buffers;
    UiInspectorWidgets::CaptureStyle(UiStyleEnumValue{std::numeric_limits<std::uint32_t>::max()}, buffers);
    std::uint32_t flags{};
    REQUIRE(buffers.integers[0].Parse(flags));
    CHECK(flags == std::numeric_limits<std::uint32_t>::max());
    buffers.assetValid = false;
    CHECK_FALSE(buffers.IsValid());
    UiInspectorWidgets::CaptureStyle(UiStyleColor{}, buffers);
    CHECK(buffers.IsValid());
}

namespace {
    /** @brief Stable long localized labels exercise shared field wrapping and sizing. */
    class LongUiLabels final : public ILocalizationService {
    public:
        const std::string &Get(std::string_view, const std::string_view key) const override {
            return messages_.try_emplace(std::string(key), "Uzun yerelleştirilmiş arayüz özelliği " + std::string(key)).first->second;
        }

    private:
        mutable std::unordered_map<std::string, std::string> messages_;
    };

    /** @brief CPU-only ImGui context owns frames and fonts; no native renderer evidence is implied. */
    struct UiFrame final {
        UiFrame() {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = {640, 900};
            io.DeltaTime = 1.0F / 60;
            io.Fonts->AddFontDefault();
            static_cast<void>(io.Fonts->Build());
        }

        ~UiFrame() {
            ImGui::DestroyContext();
        }

        UiFrame(const UiFrame &) = delete;
        UiFrame &operator=(const UiFrame &) = delete;
    };

    /** @brief Shared frame fixture keeps every context borrow within its owner lifetime. */
    struct UiRenderHarness final {
        UiFrame frame;
        Horo::EngineDataBus engine;
        EditorDataBus events;
        LongUiLabels localization;
        ImFont *font = ImGui::GetIO().Fonts->Fonts.front();
        Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font};
        ThemeContext theme{.fonts = fonts};
        EditorSettingsSnapshot settings;
        EditorGuiContext context{.engineEvents = engine,
                                 .editorEvents = events,
                                 .localization = localization,
                                 .theme = theme,
                                 .settings = settings};
    };
    enum class RenderScenario {
        Normal,
        Narrow,
        Disabled,
        InvalidText,
        OversizedGrid
    };

    /** @brief Construct all actual literal categories with explicit, syntactically valid resource identities. */
    std::array<UiStyleValue, 8> LiteralSamples() {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 9;
        const auto asset = Horo::Assets::AssetId::FromBytes(bytes);
        return {UiStyleColor{},
                UiStyleDimension{64},
                UiStyleTypography{.family = asset},
                UiStyleImage{.asset = asset},
                UiStyleShape{},
                UiStyleScalar{0.5F},
                UiStyleEnumValue{42},
                UiStyleMotionReference{}};
    }

    /** @brief Stage fixed or deliberately invalid authored counts without changing the bounded backing arrays. */
    UiLayoutStyle GridLayout(const RenderScenario scenario) {
        UiLayoutStyle layout;
        layout.container.kind = UiLayoutContainerKind::Grid;
        const auto count = scenario == RenderScenario::OversizedGrid ? std::numeric_limits<std::uint16_t>::max() : MaximumUiGridTracks;
        layout.container.columnCount = count;
        layout.container.rowCount = count;
        return layout;
    }

    /** @brief Draw the full literal sample set within one owning ImGui frame. */
    void DrawSamples(const UiRenderHarness &harness, std::array<UiStyleValue, 8> &values, std::array<UiInspectorWidgets::Buffers, 8> &text,
                     const float width, const bool disabled) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            ImGui::SetNextWindowSize({width, 850});
            ImGui::Begin("UiStyleInspectorRender", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(disabled);
            const auto previous = values[i];
            CHECK_FALSE(UiInspectorWidgets::Style(harness.context, values[i], text[i]));
            CHECK(values[i] == previous);
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::End();
        }
    }

    /** @brief Exercise real shared controls without claiming GPU or native interaction coverage. */
    void DrawControls(const UiRenderHarness &harness, UiLayoutStyle &layout, UiInspectorWidgets::Buffers &layoutText,
                      std::array<UiStyleValue, 8> &values, std::array<UiInspectorWidgets::Buffers, 8> &styleText, const float width,
                      const bool disabled) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({width, 850});
        ImGui::Begin("UiLayoutInspectorRender", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::BeginDisabled(disabled);
        CHECK_FALSE(UiInspectorWidgets::Layout(harness.context, layout, layoutText));
        ImGui::EndDisabled();
        ImGui::End();
        DrawSamples(harness, values, styleText, width, disabled);
        ImGui::Render();
        REQUIRE(ImGui::GetDrawData() != nullptr);
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    }

    /** @brief Build the same immutable selection projection consumed by the real inspector surface. */
    UiCanvasWorkspaceDocument SelectedSource() {
        SerializedUiId bytes{};
        bytes.back() = 3;
        const auto element = UiElementId::Create(bytes).Value();
        const auto instance = DocumentInstanceId::Create(1).Value();
        UiCanvasWorkspaceDocument source;
        source.identity = {.key = {DocumentKind::UiCanvas, SourceDocumentId::Parse("Hud.uicanvas").Value()}, .instance = instance};
        source.state = {1, instance};
        source.selection = element;
        source.documentRevision = UiDocumentRevision::Create(1).Value();
        source.selectedElement = CreateUiElement(UiElementKind::Canvas, element, {}).Value();
        return source;
    }

    /** @brief Passive source refreshes must never submit a document mutation. */
    void DrawSource(UiCanvasInspectorPanel &panel, const UiCanvasWorkspaceDocument &source, const UiRenderHarness &harness) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({180, 850});
        ImGui::Begin("UiCanvasInspectorHost", nullptr, ImGuiWindowFlags_NoSavedSettings);
        EditorWorkspaceViewCommandData output;
        panel.Draw(source, harness.context, output);
        CHECK(output.command == EditorWorkspaceViewCommand::None);
        CHECK_FALSE(output.uiCanvasPresentation.has_value());
        ImGui::End();
        ImGui::Render();
    }
}  // namespace

TEST_CASE("UI inspector shared layout and style controls render bounded localized and disabled frames", "[editor][ui][inspector][render]") {
    UiRenderHarness harness;
    const auto scenario = GENERATE(RenderScenario::Normal, RenderScenario::Narrow, RenderScenario::Disabled, RenderScenario::InvalidText,
                                   RenderScenario::OversizedGrid);
    const float width = scenario == RenderScenario::Narrow ? 180.0F : 360.0F;
    auto layout = GridLayout(scenario);
    UiInspectorWidgets::Buffers layoutText;
    UiInspectorWidgets::CaptureLayout(layout, layoutText);
    if (scenario == RenderScenario::InvalidText) {
        layoutText.integers[0].bytes.fill('9');
        layoutText.integers[0].valid = false;
    }
    const auto baseline = layout;
    auto values = LiteralSamples();
    std::array<UiInspectorWidgets::Buffers, 8> styleText;
    for (std::size_t i = 0; i < values.size(); ++i)
        UiInspectorWidgets::CaptureStyle(values[i], styleText[i]);
    for (int i = 0; i < 2; ++i) {
        DrawControls(harness, layout, layoutText, values, styleText, width, scenario == RenderScenario::Disabled);
        CHECK(layout == baseline);
        CHECK(layoutText.IsValid() == (scenario != RenderScenario::InvalidText));
    }
}

TEST_CASE("UI canvas inspector host surface captures and releases exact selected source frames", "[editor][ui][inspector][render]") {
    UiRenderHarness harness;
    auto source = SelectedSource();
    const auto element = source.selection;
    UiCanvasInspectorPanel panel;
    for (int i = 0; i < 4; ++i) {
        if (i == 1) {
            ++source.state.value;
            source.documentRevision = UiDocumentRevision::Create(2).Value();
        }
        if (i == 2) {
            source.selection = {};
            source.selectedElement.reset();
            ++source.selectionRevision;
        }
        if (i == 3) {
            source.selection = element;
            source.selectedElement = CreateUiElement(UiElementKind::Canvas, element, {}).Value();
            ++source.selectionRevision;
        }
        DrawSource(panel, source, harness);
    }
    panel.Clear();
}

TEST_CASE("Canonical authoring controls render localized typed fields without changing source records", "[editor][ui][inspector][render]") {
    UiRenderHarness harness;
    const auto kind = GENERATE(UiElementKind::Text, UiElementKind::Toggle, UiElementKind::Slider, UiElementKind::TextInput);
    const auto scenario = GENERATE(RenderScenario::Normal, RenderScenario::Narrow, RenderScenario::Disabled);
    const std::array arguments{UiLocalizedArgument{"integer", std::int64_t{-9007199254740993LL}},
                               UiLocalizedArgument{"decimal", 0.12345678901234567},
                               UiLocalizedArgument{"boolean", true},
                               UiLocalizedArgument{"date", UiLocalizedDateTime{123}},
                               UiLocalizedArgument{"duration", UiLocalizedDuration{456}},
                               UiLocalizedArgument{"enum", UiLocalizedStableEnum{7, 8}},
                               UiLocalizedArgument{"text", std::string{"Ürün"}},
                               UiLocalizedArgument{"shortcut", UiLocalizedShortcut{0, 65}}};
    auto message = UiLocalizedText::Create(UiMessageKey::Create("game.ui", "test").Value(), arguments, "Fallback");
    REQUIRE(message.HasValue());
    UiElementPresentation value;
    if (kind == UiElementKind::Text)
        value.localizedText = message.Value();
    if (kind == UiElementKind::Toggle)
        value.state.initial = true;
    if (kind == UiElementKind::Slider)
        value.state.initial = UiControlScalarValues{-2, 2, 0.125, 0.375};
    if (kind == UiElementKind::TextInput)
        value.state.initial = std::string{"Initial ürün"};
    value.accessibility =
        UiElementAccessibility{.role = DefaultUiElementAccessibilityRole(kind), .name = message.Value(), .description = message.Value()};
    const auto baseline = value;
    UiInspectorAuthoringWidgets controls;
    controls.Capture(value);
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({scenario == RenderScenario::Narrow ? 180.0F : 360.0F, 850});
        ImGui::Begin("UiCanonicalAuthoring", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::BeginDisabled(scenario == RenderScenario::Disabled);
        CHECK_FALSE(controls.Draw(harness.context, kind, value));
        CHECK(controls.IsValid());
        CHECK(value == baseline);
        ImGui::EndDisabled();
        ImGui::End();
        ImGui::Render();
        REQUIRE(ImGui::GetDrawData() != nullptr);
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    }
}
