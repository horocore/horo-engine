#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/UiCanvasInspectorPanel.h"
#include "editor/screens/workspace/panels/inspector/UiInspectorWidgets.h"

#include <catch2/catch_test_macros.hpp>
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
}  // namespace

TEST_CASE("UI inspector shared layout and style controls render bounded localized and disabled frames", "[editor][ui][inspector][render]") {
    UiFrame frame;
    Horo::EngineDataBus engine;
    EditorDataBus events;
    LongUiLabels localization;
    ImFont *font = ImGui::GetIO().Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font};
    const ThemeContext theme{.fonts = fonts};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engine,
                                   .editorEvents = events,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};
    float width = 360;
    bool disabled{};
    bool malformed{};
    SECTION("narrow long labels") {
        width = 180;
    }
    SECTION("disabled") {
        disabled = true;
    }
    SECTION("malformed retained text") {
        malformed = true;
    }
    UiLayoutStyle layout;
    layout.container.kind = UiLayoutContainerKind::Grid;
    layout.container.columnCount = MaximumUiGridTracks;
    layout.container.rowCount = MaximumUiGridTracks;
    UiInspectorWidgets::Buffers layoutText;
    UiInspectorWidgets::CaptureLayout(layout, layoutText);
    if (malformed) {
        layoutText.integers[0].bytes.fill('9');
        layoutText.integers[0].valid = false;
    }
    SECTION("out of range counts remain bounded during draw") {
        layout.container.columnCount = std::numeric_limits<std::uint16_t>::max();
        layout.container.rowCount = std::numeric_limits<std::uint16_t>::max();
    }
    const auto baseline = layout;
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = 9;
    const auto asset = Horo::Assets::AssetId::FromBytes(bytes);
    std::array<UiStyleValue, 8> values{UiStyleColor{},
                                       UiStyleDimension{64},
                                       UiStyleTypography{.family = asset},
                                       UiStyleImage{.asset = asset},
                                       UiStyleShape{},
                                       UiStyleScalar{0.5F},
                                       UiStyleEnumValue{42},
                                       UiStyleMotionReference{}};
    std::array<UiInspectorWidgets::Buffers, 8> styleText;
    for (std::size_t i = 0; i < values.size(); ++i)
        UiInspectorWidgets::CaptureStyle(values[i], styleText[i]);
    for (int index = 0; index != 2; ++index) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({width, 850});
        ImGui::Begin("UiLayoutInspectorRender", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::BeginDisabled(disabled);
        CHECK_FALSE(UiInspectorWidgets::Layout(context, layout, layoutText));
        ImGui::EndDisabled();
        ImGui::End();
        for (std::size_t i = 0; i < values.size(); ++i) {
            ImGui::SetNextWindowSize({width, 850});
            ImGui::Begin("UiStyleInspectorRender", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(disabled);
            const auto previous = values[i];
            CHECK_FALSE(UiInspectorWidgets::Style(context, values[i], styleText[i]));
            CHECK(values[i] == previous);
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::End();
        }
        ImGui::Render();
        REQUIRE(ImGui::GetDrawData() != nullptr);
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
        CHECK(layout == baseline);
        CHECK(layoutText.IsValid() == !malformed);
    }
}

TEST_CASE("UI canvas inspector host surface captures and releases exact selected source frames", "[editor][ui][inspector][render]") {
    UiFrame frame;
    Horo::EngineDataBus engine;
    EditorDataBus events;
    LongUiLabels localization;
    ImFont *font = ImGui::GetIO().Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font};
    const ThemeContext theme{.fonts = fonts};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engine,
                                   .editorEvents = events,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};
    Horo::Runtime::Ui::SerializedUiId bytes{};
    bytes.back() = 3;
    const auto element = Horo::Runtime::Ui::UiElementId::Create(bytes).Value();
    const auto instance = DocumentInstanceId::Create(1).Value();
    UiCanvasWorkspaceDocument source;
    source.identity = {.key = {DocumentKind::UiCanvas, SourceDocumentId::Parse("Hud.uicanvas").Value()}, .instance = instance};
    source.state = {1, instance};
    source.selection = element;
    source.documentRevision = Horo::Runtime::Ui::UiDocumentRevision::Create(1).Value();
    source.selectedElement = Horo::Runtime::Ui::CreateUiElement(Horo::Runtime::Ui::UiElementKind::Canvas, element, {}).Value();
    UiCanvasInspectorPanel panel;
    for (int i = 0; i < 4; ++i) {
        if (i == 1) {
            ++source.state.value;
            source.documentRevision = Horo::Runtime::Ui::UiDocumentRevision::Create(2).Value();
        }
        if (i == 2) {
            source.selection = {};
            source.selectedElement.reset();
            ++source.selectionRevision;
        }
        if (i == 3) {
            source.selection = element;
            source.selectedElement = Horo::Runtime::Ui::CreateUiElement(Horo::Runtime::Ui::UiElementKind::Canvas, element, {}).Value();
            ++source.selectionRevision;
        }
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({180, 850});
        ImGui::Begin("UiCanvasInspectorHost", nullptr, ImGuiWindowFlags_NoSavedSettings);
        EditorWorkspaceViewCommandData output;
        panel.Draw(source, context, output);
        CHECK(output.command == EditorWorkspaceViewCommand::None);
        CHECK_FALSE(output.uiCanvasPresentation.has_value());
        ImGui::End();
        ImGui::Render();
    }
    panel.Clear();
}
