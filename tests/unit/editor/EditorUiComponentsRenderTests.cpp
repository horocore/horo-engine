#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorSnackbarHost.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <optional>
#include <string>
#include <string_view>

namespace {
    std::string gClipboardText;

    void CaptureClipboardText(ImGuiContext *, const char *text) {
        gClipboardText = text;
    }

    /** @brief Owns a minimal Dear ImGui context and its shared test font roles. */
    struct ImGuiTestContext {
        explicit ImGuiTestContext(const ImVec2 displaySize) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            io = &ImGui::GetIO();
            io->DisplaySize = displaySize;
            io->DeltaTime = 1.0F / 60.0F;
            io->Fonts->AddFontDefault();
            static_cast<void>(io->Fonts->Build());
            ImFont *defaultFont = io->Fonts->Fonts.front();
            fonts = {.sans = defaultFont, .sansCompact = defaultFont, .sansEmphasis = defaultFont, .icon = defaultFont};
        }

        ~ImGuiTestContext() {
            ImGui::DestroyContext();
        }

        ImGuiIO *io{nullptr};
        Horo::Editor::Theme::Fonts fonts;
    };

    /** @brief Verifies that moving a modal leaves no stale content extents in its root or child windows. */
    void CheckModalScrollBounds(const std::string_view prefix) {
        for (const auto *window : ImGui::GetCurrentContext()->Windows) {
            if (!window->Active || !std::string_view{window->Name}.starts_with(prefix))
                continue;
            INFO(window->Name);
            CHECK(window->ScrollMax.x == 0.0F);
            CHECK(window->ScrollMax.y == 0.0F);
        }
    }

    struct SiblingSubmenuHoverState {
        ImVec2 firstRowCenter{};
        ImVec2 secondRowCenter{};
        bool firstOpen{false};
        bool secondOpen{false};
        bool openRoot{true};
    };

    void DrawSiblingSubmenuHoverFrame(ImGuiTestContext &imgui, SiblingSubmenuHoverState &state) {
        using namespace Horo::Editor::Ui;
        ImGui::SetNextWindowPos({20.0F, 20.0F});
        ImGui::SetNextWindowSize({240.0F, 200.0F});
        ImGui::Begin("SubmenuHoverTest");
        if (state.openRoot) {
            ImGui::OpenPopup("##root");
            state.openRoot = false;
        }
        if (BeginMenuPopup("##root")) {
            state.firstOpen = BeginContextSubmenu("Cameras###cameras", imgui.fonts);
            if (state.firstOpen) {
                static_cast<void>(ContextMenuItem("Perspective", nullptr, imgui.fonts));
                EndContextSubmenu();
            } else {
                const ImVec2 minimum = ImGui::GetItemRectMin();
                const ImVec2 maximum = ImGui::GetItemRectMax();
                state.firstRowCenter = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F};
            }
            state.secondOpen = BeginContextSubmenu("Lights###lights", imgui.fonts);
            if (state.secondOpen) {
                static_cast<void>(ContextMenuItem("Point Light", nullptr, imgui.fonts));
                EndContextSubmenu();
            } else {
                const ImVec2 minimum = ImGui::GetItemRectMin();
                const ImVec2 maximum = ImGui::GetItemRectMax();
                state.secondRowCenter = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F};
            }
            EndMenuPopup();
        }
        ImGui::End();
    }

    /** @brief Renders one complete Dear ImGui test frame. */
    template <typename DrawFrame> void RenderImGuiFrame(DrawFrame &&drawFrame) {
        ImGui::NewFrame();
        drawFrame();
        ImGui::Render();
    }

    /** @brief Sends a complete pointer click sequence while rendering each interaction state. */
    template <typename DrawFrame> void ClickImGuiItem(ImGuiIO &io, const ImVec2 position, DrawFrame &&drawFrame) {
        io.AddMousePosEvent(position.x, position.y);
        RenderImGuiFrame(drawFrame);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        RenderImGuiFrame(drawFrame);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        RenderImGuiFrame(drawFrame);
    }

    struct TooltipStyleSnapshot {
        ImVec2 originalPadding;
        ImVec2 tooltipPadding;
        ImVec2 restoredPadding;
        float originalRounding;
        float originalBorderSize;
        float tooltipRounding;
        float tooltipBorderSize;
        float restoredRounding;
        float restoredBorderSize;
        ImVec4 tooltipSurface;
        ImVec4 tooltipBorder;
    };

    [[nodiscard]] TooltipStyleSnapshot CaptureTooltipStyle() {
        using namespace Horo::Editor;
        ImGuiTestContext imgui{{320.0F, 180.0F}};
        ImGuiStyle &style = ImGui::GetStyle();
        style.WindowPadding = {1.0F, 2.0F};
        style.WindowRounding = 1.0F;
        style.PopupBorderSize = 0.0F;
        TooltipStyleSnapshot snapshot{.originalPadding = style.WindowPadding,
                                      .originalRounding = style.WindowRounding,
                                      .originalBorderSize = style.PopupBorderSize};
        RenderImGuiFrame([&] {
            ImGui::Begin("TooltipStyleTest");
            {
                Ui::ScopedTooltip tooltip(&imgui.fonts);
                snapshot.tooltipPadding = style.WindowPadding;
                snapshot.tooltipRounding = style.WindowRounding;
                snapshot.tooltipBorderSize = style.PopupBorderSize;
                snapshot.tooltipSurface = style.Colors[ImGuiCol_PopupBg];
                snapshot.tooltipBorder = style.Colors[ImGuiCol_Border];
                ImGui::TextUnformatted("Dependencies");
            }
            snapshot.restoredPadding = style.WindowPadding;
            snapshot.restoredRounding = style.WindowRounding;
            snapshot.restoredBorderSize = style.PopupBorderSize;
            ImGui::End();
        });
        return snapshot;
    }
}  // namespace

namespace Horo::Editor {
    struct EditorSnackbarHostTestAccess {
        [[nodiscard]] static const std::deque<ActiveSnackbar> &Queued(const EditorSnackbarHost &host) {
            return host.activeSnackbars_;
        }
    };
}  // namespace Horo::Editor

TEST_CASE("Snackbar host bounds queued notifications while retaining the newest entry", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;

    EditorDataBus events;
    EditorSnackbarHost host{events};
    for (std::uint64_t id = 1; id <= 64; ++id) {
        events.Publish(NotificationEvent{.id = id, .message = std::to_string(id), .durationSeconds = 0.0f});
    }

    events.Publish(NotificationEvent{.id = 65, .message = "transient", .durationSeconds = 5.0f});
    const auto &afterTransient = EditorSnackbarHostTestAccess::Queued(host);
    REQUIRE(afterTransient.size() == 64);
    REQUIRE(afterTransient.front().event.id == 2);
    REQUIRE(afterTransient.back().event.id == 65);

    events.Publish(NotificationEvent{.id = 66, .message = "persistent", .durationSeconds = 0.0f});
    const auto &afterPersistent = EditorSnackbarHostTestAccess::Queued(host);
    REQUIRE(afterPersistent.size() == 64);
    REQUIRE(afterPersistent.front().event.id == 2);
    REQUIRE(afterPersistent.back().event.id == 66);
}

TEST_CASE("Editor icon registry resolves canonical and catalog tokens", "[unit][editor][gui][design-system]") {
    using Horo::Editor::Ui::UiIcon;
    using Horo::Editor::Ui::UiIconRegistry;

    REQUIRE(UiIconRegistry::Resolve("action.delete") == UiIcon::Delete);
    REQUIRE(UiIconRegistry::Resolve("status.info") == UiIcon::Info);
    REQUIRE(UiIconRegistry::Resolve("status.warning") == UiIcon::Warning);
    REQUIRE(UiIconRegistry::Resolve("status.error") == UiIcon::Error);
    REQUIRE(UiIconRegistry::Resolve("action.more_vertical") == UiIcon::MoreVertical);
    REQUIRE(UiIconRegistry::Resolve("action.checkbox_unchecked") == UiIcon::CheckboxUnchecked);
    REQUIRE(UiIconRegistry::Resolve("primitive.light.directional") == UiIcon::DirectionalLight);
    REQUIRE(UiIconRegistry::Resolve("primitive.collider.sphere") == UiIcon::Sphere);
    REQUIRE(UiIconRegistry::Resolve("primitive.box") == UiIcon::SceneObject);
    REQUIRE(UiIconRegistry::Resolve("asset.folder") == UiIcon::Folder);
    REQUIRE(UiIconRegistry::Resolve("location.package") == UiIcon::Package);
    REQUIRE(UiIconRegistry::Resolve("navigation.arrow_back") == UiIcon::ArrowBack);
    REQUIRE_FALSE(UiIconRegistry::Resolve("unknown.icon").has_value());
    REQUIRE(std::string(UiIconRegistry::Token(UiIcon::VisibilityOff)) == "action.visibility_off");
    REQUIRE(UiIconRegistry::Token(UiIcon::SceneObject) == "scene.object");
    REQUIRE(UiIconRegistry::Token(UiIcon::None).empty());
    for (std::uint8_t value = 1; value < static_cast<std::uint8_t>(UiIcon::Count); ++value) {
        const UiIcon icon = static_cast<UiIcon>(value);
        const std::string_view token = UiIconRegistry::Token(icon);
        REQUIRE_FALSE(token.empty());
        REQUIRE(UiIconRegistry::Resolve(token) == icon);
    }

    const std::span glyphRanges = UiIconRegistry::MaterialSymbolGlyphRanges();
    REQUIRE(glyphRanges.size() >= 3U);
    REQUIRE(glyphRanges.back() == 0);
    const auto containsGlyph = [glyphRanges](const ImWchar glyph) {
        for (std::size_t index = 0; index + 1U < glyphRanges.size() && glyphRanges[index] != 0; index += 2U) {
            if (glyph >= glyphRanges[index] && glyph <= glyphRanges[index + 1U])
                return true;
        }
        return false;
    };
    REQUIRE(containsGlyph(0xE834));
    REQUIRE(containsGlyph(0xE8B8));
    REQUIRE(containsGlyph(0xE2C7));
    REQUIRE(containsGlyph(0xE5C4));
    REQUIRE(containsGlyph(0xE5C9));
    REQUIRE(containsGlyph(0xE9B0));
    REQUIRE(containsGlyph(0xEF4A));
    REQUIRE(containsGlyph(0xE145));
    REQUIRE(containsGlyph(0xE1A1));
    REQUIRE(containsGlyph(0xE88E));
    REQUIRE(containsGlyph(0xE002));
    REQUIRE(containsGlyph(0xE000));
    REQUIRE(containsGlyph(0xE872));
    REQUIRE(containsGlyph(0xF053));
}

TEST_CASE("Shared tooltip applies theme chrome and restores caller style", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    const TooltipStyleSnapshot snapshot = CaptureTooltipStyle();
    const auto &tokens = Theme::GetActiveTokens();
    REQUIRE(snapshot.tooltipPadding.x == Catch::Approx(DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Medium)));
    REQUIRE(snapshot.tooltipPadding.y == Catch::Approx(DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Small)));
    REQUIRE(snapshot.tooltipRounding == Catch::Approx(tokens.radii.card));
    REQUIRE(snapshot.tooltipBorderSize >= 1.0F);
    REQUIRE(snapshot.tooltipSurface.x == Catch::Approx(Theme::TooltipSurface().x));
    REQUIRE(snapshot.tooltipBorder.z == Catch::Approx(Theme::TooltipBorder().z));
    REQUIRE(snapshot.restoredPadding.x == Catch::Approx(snapshot.originalPadding.x));
    REQUIRE(snapshot.restoredPadding.y == Catch::Approx(snapshot.originalPadding.y));
    REQUIRE(snapshot.restoredRounding == Catch::Approx(snapshot.originalRounding));
    REQUIRE(snapshot.restoredBorderSize == Catch::Approx(snapshot.originalBorderSize));
}

TEST_CASE("Generic card title actions invoke caller-owned callbacks", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{640.0F, 480.0F}};

    bool invoked = false;
    ImVec2 actionCenter{};
    const auto drawFrame = [&] {
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({320.0F, 180.0F});
        ImGui::Begin("CardActionTest", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        {
            Card card(CardProps{.id = "##card"});
            const std::array actions{
                CardTitleBarAction{
                    .id = "reset",
                    .icon = UiIcon::Reset,
                    .title = "Reset",
                    .onInvoke =
                        [&invoked] {
                invoked = true;
            },
                },
            };
            card.DrawTitleBar({.id = "title", .title = "Component", .fonts = imgui.fonts, .actions = actions});
            const ImVec2 actionMinimum = ImGui::GetItemRectMin();
            const ImVec2 actionMaximum = ImGui::GetItemRectMax();
            actionCenter = {(actionMinimum.x + actionMaximum.x) * 0.5F, (actionMinimum.y + actionMaximum.y) * 0.5F};
            if (card.BeginBody())
                ImGui::TextUnformatted("Body");
        }
        ImGui::End();
    };

    RenderImGuiFrame(drawFrame);
    ClickImGuiItem(*imgui.io, actionCenter, drawFrame);

    REQUIRE(invoked);
}

TEST_CASE("Generic card disclosure persists and suppresses collapsed body content", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{400.0F, 240.0F}};

    bool bodyVisible = false;
    ImVec2 disclosureCenter{};
    const auto drawFrame = [&] {
        bodyVisible = false;
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({320.0F, 180.0F});
        ImGui::Begin("CardDisclosureTest", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        {
            Card card(CardProps{.id = "##card"});
            card.DrawTitleBar({.id = "title", .title = "Camera", .fonts = imgui.fonts});
            const ImVec2 disclosureMinimum = ImGui::GetItemRectMin();
            const ImVec2 disclosureMaximum = ImGui::GetItemRectMax();
            disclosureCenter = {(disclosureMinimum.x + disclosureMaximum.x) * 0.5F, (disclosureMinimum.y + disclosureMaximum.y) * 0.5F};
            bodyVisible = card.BeginBody();
            if (bodyVisible)
                ImGui::TextUnformatted("Projection");
        }
        ImGui::End();
    };

    RenderImGuiFrame(drawFrame);
    REQUIRE(bodyVisible);

    ClickImGuiItem(*imgui.io, disclosureCenter, drawFrame);
    REQUIRE_FALSE(bodyVisible);

    RenderImGuiFrame(drawFrame);
    REQUIRE_FALSE(bodyVisible);
}

TEST_CASE("Side dock tabs preserve reference padding height and interaction", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    Theme::SetUiScalePercent(100);
    ImGuiTestContext imgui{{400.0F, 200.0F}};
    ImFont *defaultFont = imgui.fonts.sans;
    const std::array<const char *, 2> tabs{"Inspector", "Scene"};

    int activeTab = 0;
    float startY = 0.0F;
    float endY = 0.0F;
    ImVec2 sceneTabCenter{};
    const auto drawFrame = [&] {
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({300.0F, 160.0F});
        ImGui::Begin("SideDockTabsTest", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        startY = ImGui::GetCursorScreenPos().y;
        activeTab = DrawSideDockTabs(tabs, activeTab, imgui.fonts);
        endY = ImGui::GetCursorScreenPos().y;
        const float tabFontSize = Theme::TextPx::Label();
        const float inspectorWidth = defaultFont->CalcTextSizeA(tabFontSize, 100000.0F, 0.0F, tabs.front()).x + 20.0F;
        const float sceneWidth = defaultFont->CalcTextSizeA(tabFontSize, 100000.0F, 0.0F, tabs.back()).x + 20.0F;
        sceneTabCenter = {ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x + 10.0F + inspectorWidth + sceneWidth * 0.5F,
                          startY + 18.0F};
        ImGui::End();
    };

    RenderImGuiFrame(drawFrame);
    REQUIRE(endY - startY == Catch::Approx(36.0F));

    ClickImGuiItem(*imgui.io, sceneTabCenter, drawFrame);
    REQUIRE(activeTab == 1);
}

TEST_CASE("Workspace popup rows keep the design-system menu geometry", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{640.0F, 480.0F}};

    ImGui::NewFrame();
    ImGui::Begin("PopupGeometryTest");
    ImGui::OpenPopup("##popup");
    REQUIRE(BeginMenuPopup("##popup"));
    static_cast<void>(ContextMenuItem("Create", nullptr, imgui.fonts));
    const float rowHeight = ImGui::GetItemRectSize().y;
    const float popupWidth = ImGui::GetWindowWidth();
    ImGui::OpenPopup("##submenu_popup_GameObject###submenu");
    REQUIRE(BeginContextSubmenu("GameObject###submenu", imgui.fonts));
    static_cast<void>(ContextMenuItem("Box", nullptr, imgui.fonts));
    EndContextSubmenu();
    EndMenuPopup();
    ImGui::End();
    ImGui::Render();

    REQUIRE(rowHeight == Catch::Approx(30.0F));
    REQUIRE(popupWidth >= 176.0F);
    REQUIRE(popupWidth < 224.0F);
}

TEST_CASE("Hovering a sibling context submenu switches its children without clicking", "[unit][editor][gui][design-system]") {
    ImGuiTestContext imgui{{640.0F, 480.0F}};
    SiblingSubmenuHoverState state;

    const auto drawFrame = [&] {
        DrawSiblingSubmenuHoverFrame(imgui, state);
    };

    RenderImGuiFrame(drawFrame);
    imgui.io->AddMousePosEvent(state.firstRowCenter.x, state.firstRowCenter.y);
    RenderImGuiFrame(drawFrame);
    RenderImGuiFrame(drawFrame);
    REQUIRE(state.firstOpen);

    imgui.io->AddMousePosEvent(state.secondRowCenter.x, state.secondRowCenter.y);
    RenderImGuiFrame(drawFrame);
    REQUIRE(state.secondOpen);
    RenderImGuiFrame(drawFrame);
    REQUIRE_FALSE(state.firstOpen);
}

TEST_CASE("Menu-bar dropdowns reuse workspace popup rows", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{640.0F, 480.0F}};

    ImGui::NewFrame();
    ImGui::Begin("MenuBarDropdownTest", nullptr, ImGuiWindowFlags_MenuBar);
    REQUIRE(ImGui::BeginMenuBar());
    ImGui::OpenPopup("Window");
    REQUIRE(BeginMenuDropdown("Window", imgui.fonts));
    static_cast<void>(ContextMenuItem("Workspace", nullptr, imgui.fonts));
    const float rowHeight = ImGui::GetItemRectSize().y;
    const float popupWidth = ImGui::GetWindowWidth();
    EndMenuDropdown();
    ImGui::EndMenuBar();
    ImGui::End();
    ImGui::Render();

    REQUIRE(rowHeight == Catch::Approx(30.0F));
    REQUIRE(popupWidth >= 176.0F);
    REQUIRE(popupWidth < 224.0F);
}

TEST_CASE("Component metrics use theme overrides while global scaling is disabled", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::DesignSystem;

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "horo-component-token-theme.json";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << R"({
            "name": "Component token test",
            "tokens": {
                "typography": {"sansCompactBase": 12, "caption": 12, "label": 12},
                "componentSizes": {
                    "xs": {
                        "fontSize": 11,
                        "paddingX": 6,
                        "paddingY": 2,
                        "minimumHeight": 20,
                        "iconSize": 10
                    }
                },
                "styleSpacing": {"xs": 3, "s": 7, "m": 13, "l": 19, "xl": 29}
            }
        })";
    }

    Theme::ThemeEntry entry;
    REQUIRE(Theme::LoadThemeFromJson(path.string().c_str(), entry));
    REQUIRE(entry.designTokens.typography.sansCompactBase == 16.0F);
    REQUIRE(entry.designTokens.typography.caption == 16.0F);
    REQUIRE(entry.designTokens.typography.label == 16.0F);
    const ComponentSizeMetrics &xs = MetricsFor(entry.designTokens, ComponentSize::XS);
    REQUIRE(xs.fontSize == 16.0F);
    REQUIRE(xs.minimumHeight == 20.0F);
    REQUIRE(SpacingFor(entry.designTokens, SpacingSize::Medium) == 13.0F);
    std::error_code removeError;
    std::filesystem::remove(path, removeError);

    Theme::SetUiScalePercent(120);
    REQUIRE(MetricsFor(Theme::GetActiveTokens(), ComponentSize::XS).minimumHeight == Catch::Approx(24.0F));
    Theme::SetUiScalePercent(130);
    REQUIRE(MetricsFor(Theme::GetActiveTokens(), ComponentSize::XS).minimumHeight == Catch::Approx(24.0F));
}

TEST_CASE("Small toolbar primitives share height and fixed action width", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    Theme::SetUiScalePercent(100);
    ImGuiTestContext imgui{{800.0F, 240.0F}};
    ImFont *defaultFont = imgui.fonts.sans;

    ImVec2 inputSize{};
    ImVec2 buttonSize{};
    ImVec2 comboSize{};
    ImVec2 multiSelectSize{};
    std::array<char, 32> search{};
    int status = 0;
    std::array<bool, 3> visible{true, true, true};
    const std::array<const char *, 4> statuses{"All Status", "OK", "Failed", "Cached"};
    const std::array<const char *, 3> columns{"Status", "Operation", "Message"};

    ImGui::NewFrame();
    ImGui::Begin("ToolbarPrimitiveGeometry");
    static_cast<void>(InputTextControl("##Search", search.data(), search.size(), imgui.fonts,
                                       InputTextOptions{.width = 180.0F, .hint = "Filter...", .componentSize = ComponentSize::Small}));
    inputSize = ImGui::GetItemRectSize();
    ImGui::SameLine();
    static_cast<void>(Button({.label = "Clear",
                              .size = {104.0F, 0.0F},
                              .variant = ButtonVariant::Secondary,
                              .font = defaultFont,
                              .baseFontSize = Theme::FontPx::SansCompact,
                              .componentSize = ComponentSize::Small}));
    buttonSize = ImGui::GetItemRectSize();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(104.0F);
    static_cast<void>(ComboControl("##Status", &status, statuses.data(), static_cast<int>(statuses.size()), imgui.fonts,
                                   ComboControlOptions{.componentSize = ComponentSize::Small}));
    comboSize = ImGui::GetItemRectSize();
    ImGui::SameLine();
    static_cast<void>(MultiSelectField("##Columns", "Columns", columns, visible, imgui.fonts, 104.0F, ComponentSize::Small));
    multiSelectSize = ImGui::GetItemRectSize();
    ImGui::End();
    ImGui::Render();

    REQUIRE(buttonSize.x == Catch::Approx(104.0F));
    REQUIRE(comboSize.x == Catch::Approx(buttonSize.x));
    REQUIRE(multiSelectSize.x == Catch::Approx(buttonSize.x));
    REQUIRE(inputSize.y == Catch::Approx(buttonSize.y).margin(0.1F));
    REQUIRE(comboSize.y == Catch::Approx(buttonSize.y).margin(0.1F));
    REQUIRE(multiSelectSize.y == Catch::Approx(buttonSize.y).margin(0.1F));
}

TEST_CASE("Modal text input matches the requested combo height", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    Theme::SetUiScalePercent(100);
    ImGuiTestContext imgui{{800.0F, 240.0F}};
    std::array<char, 32> filter{};
    const std::array<const char *, 1> choices{"Project"};
    int selection = 0;
    ImVec2 inputSize{};
    ImVec2 comboSize{};

    ImGui::NewFrame();
    ImGui::Begin("ModalFieldGeometry");
    static_cast<void>(InputTextControl("##Filter", filter.data(), filter.size(), imgui.fonts,
                                       {.width = 250.0F, .height = 36.0F, .componentSize = ComponentSize::Medium}));
    inputSize = ImGui::GetItemRectSize();
    static_cast<void>(ComboControl("##Source", &selection, choices.data(), static_cast<int>(choices.size()), imgui.fonts,
                                   {.height = 36.0F, .componentSize = ComponentSize::Medium}));
    comboSize = ImGui::GetItemRectSize();
    ImGui::End();
    ImGui::Render();

    REQUIRE(inputSize.y == Catch::Approx(comboSize.y).margin(0.1F));
}

TEST_CASE("Shared modal shell composes badge split panes and fixed footer", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{1280.0F, 720.0F}};

    ImGui::NewFrame();
    bool drewLeading = false;
    bool drewContent = false;
    bool preservedContentSpacing = false;
    ImGui::GetStyle().ItemSpacing = {13.0F, 9.0F};
    {
        ScopedModalShell modal(
            {
                .id = "ComponentModalShellTest",
                .title = "Component Modal",
                .requestedSize = {900.0F, 640.0F},
                .headerHeight = 56.0F,
                .footerHeight = 64.0F,
            },
            imgui.fonts);
        REQUIRE((modal.BodyHeight() > 0.0F));
        REQUIRE((modal.FooterStartY() > modal.BodyHeight()));
        REQUIRE_FALSE(modal.CloseRequested());

        const BadgeProps smallBadge{
            .label = "Stable",
            .tone = BadgeTone::Success,
        };
        const BadgeProps statusPill{
            .label = "Installing",
            .tone = BadgeTone::Accent,
            .size = BadgeSize::Medium,
            .leadingIndicator = true,
        };
        REQUIRE((BadgeWidth(statusPill, imgui.fonts) > BadgeWidth(smallBadge, imgui.fonts)));

        ModalSplitPane(
            {
                .id = "ComponentModalSplitTest",
                .size = {0.0F, modal.BodyHeight()},
                .leadingWidth = 280.0F,
            },
            [&]() {
            drewLeading = true;
            Badge(smallBadge, imgui.fonts);
        }, [&]() {
            drewContent = true;
            const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
            preservedContentSpacing = spacing.x == 13.0F && spacing.y == 9.0F;
            Badge(statusPill, imgui.fonts);
        });
        modal.BeginFooter({20.0F, 12.0F});
        ImGui::TextUnformatted("Footer");
        modal.EndFooter();
    }
    ImGui::Render();

    REQUIRE(drewLeading);
    REQUIRE(drewContent);
    REQUIRE(preservedContentSpacing);
}

TEST_CASE("Shared modal shell keeps its actual children inside narrow and resized placement regions",
          "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{1280.0F, 720.0F}};
    std::optional<ModalPlacementRegion> region;
    const auto draw = [&] {
        ScopedModalShell modal({.id = "NarrowModalShellTest",
                                .title = "Long localized navigation operation confirmation title",
                                .requestedSize = {640.0F, 480.0F},
                                .placementRegion = region},
                               imgui.fonts);
        ImGui::BeginChild("##Body", {0.0F, modal.BodyHeight()}, false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        Hint("Long modal content remains readable and scrolls inside its own body.", imgui.fonts);
        ImGui::EndChild();
        modal.BeginFooter({8.0F, 8.0F});
        static_cast<void>(Button({.label = "Confirm", .font = imgui.fonts.sans, .style = {.width = StyleWidth::FillAvailable}}));
        modal.EndFooter();
    };
    RenderImGuiFrame(draw);
    imgui.io->DisplaySize = {320.0F, 600.0F};
    for (int placement = 0; placement < 2; ++placement) {
        if (placement != 0)
            region = ModalPlacementRegion{{10.0F, 20.0F}, {300.0F, 450.0F}};
        RenderImGuiFrame(draw);
        RenderImGuiFrame(draw);
        const ImVec2 minimum = region ? region->position : ImGui::GetMainViewport()->WorkPos;
        const ImVec2 size = region ? region->size : ImGui::GetMainViewport()->WorkSize;
        int children{};
        for (const auto *window : ImGui::GetCurrentContext()->Windows) {
            if (!window->Active || std::string_view{window->Name}.find("NarrowModalShellTest") != 0)
                continue;
            INFO(window->Name);
            CHECK(window->Pos.x >= minimum.x);
            CHECK(window->Pos.y >= minimum.y);
            CHECK(window->Pos.x + window->Size.x <= minimum.x + size.x);
            CHECK(window->Pos.y + window->Size.y <= minimum.y + size.y);
            CHECK(window->ScrollMax.x == 0.0F);
            if (std::string_view{window->Name}.find("##ModalHeader") != std::string_view::npos)
                CHECK(window->ScrollMax.y == 0.0F);
            ++children;
        }
        CHECK(children == 4);
    }
}

TEST_CASE("Shared text controls keep long opaque and localized text inside narrow scrolling content",
          "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{320.0F, 600.0F}};
    const std::array<std::string, 2> samples{
        std::string(180, 'W'),
        "Kaynak sinirlari: eszamanli isler / yerlesik bayt / gecici bayt ve uzun profil bilgileri",
    };
    const auto draw = [&] {
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({320.0F, 600.0F});
        ImGui::Begin("NarrowTextTest", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::BeginChild("##Text", {280.0F, 180.0F}, false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (const auto &text : samples) {
            Hint(text.c_str(), imgui.fonts);
            CHECK(ImGui::GetItemRectMax().x <= right);
            ErrorText(text.c_str(), imgui.fonts);
            CHECK(ImGui::GetItemRectMax().x <= right);
            WrappedText(ImGui::GetContentRegionAvail().x, text, Theme::Text(), imgui.fonts);
            CHECK(ImGui::GetItemRectMax().x <= right);
        }
        ImGui::EndChild();
        ImGui::End();
    };
    RenderImGuiFrame(draw);
    RenderImGuiFrame(draw);
    for (const auto *window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active) {
            INFO(window->Name);
            CHECK(window->ScrollMax.x == 0.0F);
        }
    }
}

TEST_CASE("Shared modal shell close icon requests dismissal", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{1280.0F, 720.0F}};
    const auto draw = [&] {
        ImGui::NewFrame();
        bool closeRequested = false;
        {
            ScopedModalShell modal({.id = "ModalCloseTest",
                                    .title = "Import Assets",
                                    .requestedSize = {1000.0F, 690.0F},
                                    .headerHeight = 38.0F,
                                    .footerHeight = 68.0F},
                                   imgui.fonts);
            closeRequested = modal.CloseRequested();
        }
        ImGui::Render();
        return closeRequested;
    };

    REQUIRE_FALSE(draw());
    const ImGuiWindow *window = ImGui::FindWindowByName("ModalCloseTest");
    REQUIRE(window != nullptr);
    imgui.io->AddMousePosEvent(window->Pos.x + window->Size.x - 36.0F, window->Pos.y + 19.0F);
    imgui.io->AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    REQUIRE_FALSE(draw());
    imgui.io->AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    REQUIRE(draw());
}

TEST_CASE("Shared modal shell supports full-header dragging and remains inside the work area", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{1280.0F, 720.0F}};
    const auto drawModal = [&] {
        ScopedModalShell modal(
            {
                .id = "MovableModalShellTest",
                .title = "Movable Modal",
                .requestedSize = {640.0F, 480.0F},
                .headerHeight = 48.0F,
            },
            imgui.fonts);
    };

    RenderImGuiFrame(drawModal);
    const ImGuiWindow *window = ImGui::FindWindowByName("MovableModalShellTest");
    REQUIRE(window != nullptr);
    const ImVec2 initialPosition = window->Pos;

    ImGuiIO &io = *imgui.io;
    // Begin over the visible title text rather than the empty middle of the header.
    io.AddMousePosEvent(initialPosition.x + 28.0F, initialPosition.y + 24.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    RenderImGuiFrame(drawModal);
    io.AddMousePosEvent(-400.0F, -300.0F);
    RenderImGuiFrame(drawModal);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    RenderImGuiFrame(drawModal);

    window = ImGui::FindWindowByName("MovableModalShellTest");
    REQUIRE(window != nullptr);
    REQUIRE(window->Pos.x == Catch::Approx(ImGui::GetMainViewport()->WorkPos.x));
    REQUIRE(window->Pos.y == Catch::Approx(ImGui::GetMainViewport()->WorkPos.y));
    CheckModalScrollBounds("MovableModalShellTest");

    // The right side of the header remains draggable outside the close action.
    io.AddMousePosEvent(window->Pos.x + window->Size.x - 80.0F, window->Pos.y + 24.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    RenderImGuiFrame(drawModal);
    io.AddMousePosEvent(2000.0F, 1400.0F);
    RenderImGuiFrame(drawModal);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    RenderImGuiFrame(drawModal);

    window = ImGui::FindWindowByName("MovableModalShellTest");
    REQUIRE(window != nullptr);
    const ImVec2 workMaximum{ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x - window->Size.x,
                             ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y - window->Size.y};
    REQUIRE(window->Pos.x == Catch::Approx(workMaximum.x));
    REQUIRE(window->Pos.y == Catch::Approx(workMaximum.y));
    CheckModalScrollBounds("MovableModalShellTest");
}

TEST_CASE("Selectable text block copies a selection spanning multiple lines", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{640.0F, 360.0F}};
    ImGuiIO &io = *imgui.io;
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = CaptureClipboardText;

    std::string text{"first line\nsecond line\nthird line"};
    const std::array lineLayouts{
        SelectableTextLineLayout{
            .color = {1.0F, 0.2F, 0.2F, 1.0F},
            .alignedColumnByteOffset = 6U,
        },
        SelectableTextLineLayout{
            .color = {0.2F, 1.0F, 0.2F, 1.0F},
            .alignedColumnByteOffset = 7U,
        },
        SelectableTextLineLayout{
            .color = {0.2F, 0.2F, 1.0F, 1.0F},
            .alignedColumnByteOffset = 6U,
        },
    };
    ImVec2 textOrigin{};
    bool textBlockActive = false;
    const auto drawBlock = [&]() {
        ImGui::SetNextWindowPos({20.0F, 20.0F});
        ImGui::SetNextWindowSize({500.0F, 240.0F});
        ImGui::Begin("SelectableTextBlockTest");
        textBlockActive = SelectableTextBlock("##LogText", text.data(), text.size() + 1U, lineLayouts, 100.0F);
        textOrigin = ImGui::GetItemRectMin();
        ImGui::End();
    };

    ImGui::NewFrame();
    drawBlock();
    ImGui::Render();

    io.AddMousePosEvent(textOrigin.x + 1.0F, textOrigin.y + 3.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ImGui::NewFrame();
    drawBlock();
    ImGui::Render();
    REQUIRE(textBlockActive);

    io.AddMousePosEvent(textOrigin.x + ImGui::CalcTextSize("third line").x, textOrigin.y + ImGui::GetFontSize() * 2.0F + 3.0F);
    ImGui::NewFrame();
    drawBlock();
    ImGui::Render();

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ImGui::NewFrame();
    drawBlock();
    ImGui::Render();

    gClipboardText.clear();
    io.AddKeyEvent(io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_C, true);
    ImGui::NewFrame();
    drawBlock();
    ImGui::Render();

    REQUIRE(gClipboardText == text);
}

TEST_CASE("Editable object title keeps its compact input vertically centered", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    ImGuiTestContext imgui{{480.0F, 240.0F}};

    ImGui::NewFrame();
    ImGui::SetNextWindowPos({20.0F, 20.0F});
    ImGui::SetNextWindowSize({320.0F, 160.0F});
    ImGui::Begin("EditableObjectTitleTest");
    const ImVec2 titleOrigin = ImGui::GetCursorScreenPos();
    std::string value{"Box"};
    static_cast<void>(
        DrawEditableTitle("object_name", value, 128U, imgui.fonts, {.leadingIcon = UiIcon::HierarchyMesh, .trailingWidth = 88.0F}));
    const ImVec2 inputMinimum = ImGui::GetItemRectMin();
    const ImVec2 inputMaximum = ImGui::GetItemRectMax();
    ImGui::End();
    ImGui::Render();

    constexpr float titleHeight = 38.0F;
    const float topPadding = inputMinimum.y - titleOrigin.y;
    const float bottomPadding = titleOrigin.y + titleHeight - inputMaximum.y;
    INFO("top padding: " << topPadding << ", bottom padding: " << bottomPadding);
    REQUIRE((inputMaximum.y - inputMinimum.y == Catch::Approx(30.0F).margin(1.0F)));
    REQUIRE((std::fabs(topPadding - bottomPadding) <= 1.0F));
}

TEST_CASE("Inspector property rows use the theme vertical gap", "[unit][editor][gui][design-system]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::Ui;

    Theme::SetUiScalePercent(100);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {480.0F, 240.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());
    ImFont *defaultFont = io.Fonts->Fonts.front();
    const Theme::Fonts fonts{
        .sans = defaultFont,
        .sansCompact = defaultFont,
        .sansEmphasis = defaultFont,
    };

    ImGui::NewFrame();
    ImGui::SetNextWindowPos({20.0F, 20.0F});
    ImGui::SetNextWindowSize({360.0F, 180.0F});
    ImGui::Begin("InspectorPropertyRowGapTest");
    float firstValue = 1.0F;
    float secondValue = 2.0F;
    static_cast<void>(DrawFloatPropRow("First", "first", firstValue, fonts));
    const float firstControlBottom = ImGui::GetItemRectMax().y;
    static_cast<void>(DrawFloatPropRow("Second", "second", secondValue, fonts));
    const float secondControlTop = ImGui::GetItemRectMin().y;
    ImGui::End();
    ImGui::Render();

    const float controlGap = secondControlTop - firstControlBottom;
    REQUIRE(controlGap == Catch::Approx(Theme::GetActiveTokens().spacing.propertyRowGap).margin(1.0F));

    ImGui::DestroyContext();
}

TEST_CASE("Document tabs reserve trailing close padding and distinguish focus from close",
          "[unit][editor][gui][design-system][source-tab]") {
    using namespace Horo::Editor;
    Theme::SetUiScalePercent(100);
    ImGuiTestContext gui{{800, 260}};
    Ui::DocumentTabAction action{};
    ImRect bounds;
    ImGuiWindow *window{};
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({780, 240});
        ImGui::Begin("Document tab geometry");
        action = Ui::DocumentTabButton("document-1", "LongLocalizedDocumentName.horo_script *", "Close", true, gui.fonts);
        bounds = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        window = ImGui::GetCurrentWindow();
        ImGui::End();
        ImGui::Render();
    };
    frame();
    frame();
    const auto &tokens = Theme::GetActiveTokens();
    const float padding = DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::XS);
    const float height = Ui::DocumentTabHeight();
    const auto scope = window->GetID("document-1");
    const auto closeScope = ImHashStr("##close", 0, scope);
    const auto closeId = ImHashStr("##close", 0, closeScope);
    ImGui::SetFocusID(closeId, window);
    frame();
    CHECK(bounds.GetWidth() == Catch::Approx(160.0F));
    CHECK(bounds.GetHeight() == Catch::Approx(height));
    const ImVec2 closeCenter{bounds.Max.x - padding - 12.0F, (bounds.Min.y + bounds.Max.y) * 0.5F};
    gui.io->AddMousePosEvent(closeCenter.x, closeCenter.y);
    frame();
    gui.io->AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    gui.io->AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    CHECK(action == Ui::DocumentTabAction::Close);
    gui.io->AddMousePosEvent(bounds.Min.x + padding + 5, closeCenter.y);
    frame();
    gui.io->AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    gui.io->AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    CHECK(action == Ui::DocumentTabAction::Focus);
}

TEST_CASE("Pinned Viewport and closable source tabs share height typography and active chrome",
          "[unit][editor][gui][design-system][source-tab]") {
    using namespace Horo::Editor;
    Theme::SetUiScalePercent(100);
    ImGuiTestContext gui{{850, 260}};
    ImVec2 pinnedSize{}, sourceSize{};
    ImVec2 pinnedMin{}, pinnedMax{};
    Ui::DocumentTabAction pinnedAction{};
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({830, 240});
        ImGui::Begin("Unified document tabs");
        pinnedAction = Ui::DocumentTabButton("viewport", "Viewport", "Close", true, gui.fonts, false);
        pinnedMin = ImGui::GetItemRectMin();
        pinnedMax = ImGui::GetItemRectMax();
        pinnedSize = ImGui::GetItemRectSize();
        ImGui::SameLine(0, 0);
        static_cast<void>(Ui::DocumentTabButton("source", "Jump.horo_script", "Close", false, gui.fonts));
        sourceSize = ImGui::GetItemRectSize();
        ImGui::End();
        ImGui::Render();
    };
    frame();
    frame();
    REQUIRE(pinnedSize.x == sourceSize.x);
    REQUIRE(pinnedSize.y == sourceSize.y);
    REQUIRE(sourceSize.y == Ui::DocumentTabHeight());
    gui.io->AddMousePosEvent(pinnedMax.x - 5, (pinnedMin.y + pinnedMax.y) * 0.5F);
    frame();
    gui.io->AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    gui.io->AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    REQUIRE(pinnedAction == Ui::DocumentTabAction::Focus);
}

TEST_CASE("Package theme identities survive list growth and withdraw without retaining active provider data",
          "[editor][theme][extensions]") {
    using namespace Horo::Editor;
    ImGuiTestContext gui{{800, 260}};
    Theme::RefreshThemeList();
    Theme::ThemeEntry theme;
    theme.id = "example.editor.theme";
    theme.name = "Example";
    theme.isBuiltIn = false;
    REQUIRE(Theme::RegisterPackageTheme(theme));
    CHECK_FALSE(Theme::RegisterPackageTheme(theme));
    REQUIRE(Theme::SelectThemeById(theme.id));
    CHECK(Theme::GetThemeList()[static_cast<std::size_t>(Theme::GetActiveThemeIndex())].id == theme.id);
    auto refreshed = theme;
    refreshed.id.clear();
    REQUIRE(Theme::ApplyReloadedTheme(std::move(refreshed)));
    CHECK(Theme::GetThemeList()[static_cast<std::size_t>(Theme::GetActiveThemeIndex())].id == theme.id);
    Theme::UnregisterPackageTheme(theme.id);
    CHECK_FALSE(Theme::SelectThemeById(theme.id));
    CHECK(Theme::GetActiveThemeIndex() == 0);
}

TEST_CASE("Descriptionless settings rows center labels and controls with even vertical padding", "[editor][gui][settings][layout]") {
    using namespace Horo::Editor;
    ImGuiTestContext gui{{1000, 650}};
    Theme::RefreshThemeList();
    for (const int theme : {0, 2}) {
        Theme::SelectThemeByIndex(theme);
        for (const float width : {440.0F, 940.0F}) {
            std::array<ImRect, 2> rows{}, controls{};
            const auto frame = [&] {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({width, 620});
                ImGui::Begin("Settings row geometry", nullptr, ImGuiWindowFlags_NoSavedSettings);
                for (std::size_t index = 0; index < rows.size(); ++index) {
                    ImGui::PushID(static_cast<int>(index));
                    Ui::SettingRow("Save", index == 0 ? "" : nullptr, gui.fonts, [&] {
                        static_cast<void>(Ui::Button(
                            {.label = "Cmd + S", .variant = Ui::ButtonVariant::Secondary, .enabled = index == 0, .font = gui.fonts.sans}));
                        controls[index] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                        ImGui::SameLine();
                        static_cast<void>(Ui::Button(
                            {.label = "Reset", .variant = Ui::ButtonVariant::Secondary, .enabled = index == 0, .font = gui.fonts.sans}));
                    });
                    rows[index] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                    ImGui::PopID();
                }
                ImGui::End();
                ImGui::Render();
            };
            frame();
            frame();
            for (std::size_t index = 0; index < rows.size(); ++index) {
                CHECK(rows[index].GetCenter().y == Catch::Approx(controls[index].GetCenter().y).margin(0.5F));
                CHECK(controls[index].Min.y - rows[index].Min.y == Catch::Approx(rows[index].Max.y - controls[index].Max.y).margin(0.5F));
            }
            CHECK(rows[0].Max.y == Catch::Approx(rows[1].Min.y).margin(0.5F));
            const auto color = Theme::U32(Theme::Text());
            ImRect labelInk{{10000, 10000}, {-10000, -10000}}, buttonInk = labelInk;
            const auto *data = ImGui::GetDrawData();
            for (int list = 0; list < data->CmdListsCount; ++list)
                for (const auto &vertex : data->CmdLists[list]->VtxBuffer) {
                    if (vertex.col != color || vertex.pos.y < rows[0].Min.y || vertex.pos.y > rows[0].Max.y)
                        continue;
                    if (vertex.pos.x >= rows[0].Min.x && vertex.pos.x < controls[0].Min.x)
                        labelInk.Add(vertex.pos);
                    else if (controls[0].Contains(vertex.pos))
                        buttonInk.Add(vertex.pos);
                }
            REQUIRE(labelInk.GetHeight() > 0);
            REQUIRE(buttonInk.GetHeight() > 0);
            CHECK(labelInk.GetCenter().y == Catch::Approx(buttonInk.GetCenter().y).margin(1.0F));
        }
    }
    Theme::SelectThemeByIndex(0);
}

TEST_CASE("Settings descriptions remain smaller than labels with compact bounded row spacing", "[editor][gui][settings][layout]") {
    using namespace Horo::Editor;
    ImGuiTestContext gui{{1100, 700}};
    Theme::RefreshThemeList();
    for (const int theme : {0, 2}) {
        Theme::SelectThemeByIndex(theme);
        for (const int scale : {100, 150}) {
            Theme::SetUiScalePercent(scale);
            for (const float width : {640.0F, 1000.0F}) {
                ImRect row, control;
                const auto frame = [&] {
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos({0, 0});
                    ImGui::SetNextWindowSize({width, 650});
                    ImGui::Begin("Compact settings", nullptr, ImGuiWindowFlags_NoSavedSettings);
                    Ui::SettingRow("Setting label", "Supporting hint", gui.fonts, [&] {
                        static_cast<void>(Ui::Button({.label = "Value", .variant = Ui::ButtonVariant::Secondary, .font = gui.fonts.sans}));
                        control = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                    });
                    row = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                    ImGui::End();
                    ImGui::Render();
                };
                frame();
                frame();
                ImRect label{{10000, 10000}, {-10000, -10000}}, hint = label;
                const auto *data = ImGui::GetDrawData();
                for (int list = 0; list < data->CmdListsCount; ++list)
                    for (const auto &vertex : data->CmdLists[list]->VtxBuffer) {
                        if (vertex.pos.x >= control.Min.x || vertex.pos.y < row.Min.y || vertex.pos.y > row.Max.y)
                            continue;
                        if (vertex.col == Theme::U32(Theme::Text()))
                            label.Add(vertex.pos);
                        if (vertex.col == Theme::U32(Theme::Dim()))
                            hint.Add(vertex.pos);
                    }
                REQUIRE(label.GetHeight() > 0);
                REQUIRE(hint.GetHeight() > 0);
                CHECK(hint.GetHeight() < label.GetHeight());
                CHECK(hint.Min.y > label.Max.y);
                CHECK(hint.Max.x < control.Min.x);
                const auto metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Small);
                CHECK(control.GetHeight() >= metrics.minimumHeight);
                // One label and one hint stay compact; narrow scaled columns may wrap.
                if (width == 1000.0F)
                    CHECK(row.GetHeight() <= 60.0F * static_cast<float>(scale) / 100.0F);
            }
        }
    }
    Theme::SetUiScalePercent(100);
    Theme::SelectThemeByIndex(0);
}

TEST_CASE("Context menus measure visible labels and bound long component names to the popup", "[editor][gui][context-menu]") {
    using namespace Horo::Editor;
    for (const float width : {320.0F, 900.0F}) {
        ImGuiTestContext gui{{width, 600}};
        for (const char *label : {"A reasonably long component name###stable",
                                  "An extremely long module supplied behavior name that cannot fit on one row###stable"}) {
            ImRect popup;
            ImDrawList *draw{};
            for (int frame = 0; frame < 3; ++frame) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({width, 580});
                ImGui::Begin("Component menu bounds", nullptr, ImGuiWindowFlags_NoSavedSettings);
                if (frame == 0)
                    ImGui::OpenPopup("##component-bounds");
                REQUIRE(Ui::BeginMenuPopup("##component-bounds"));
                static_cast<void>(Ui::ContextMenuItem(label, nullptr, gui.fonts));
                popup = {ImGui::GetWindowPos(),
                         {ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), ImGui::GetWindowPos().y + ImGui::GetWindowHeight()}};
                draw = ImGui::GetWindowDrawList();
                CHECK(ImGui::GetCurrentWindow()->ScrollMax.x == 0);
                Ui::EndMenuPopup();
                ImGui::End();
                ImGui::Render();
            }
            REQUIRE(draw);
            CHECK(popup.GetWidth() > 176);
            CHECK(popup.GetWidth() <= std::min(width - 16.0F, 340.0F));
            for (const auto &vertex : draw->VtxBuffer)
                if (vertex.col == Theme::U32(Theme::Text())) {
                    CHECK(vertex.pos.x >= popup.Min.x);
                    CHECK(vertex.pos.x <= popup.Max.x - 5.0F);
                }
        }
    }
}

TEST_CASE("Context submenus flip at the right edge and remain inside the viewport", "[editor][gui][context-menu][submenu-placement]") {
    using namespace Horo::Editor;
    for (const bool rightEdge : {false, true}) {
        for (const float y : {40.0F, 480.0F}) {
            ImGuiTestContext gui{{900, 600}};
            ImRect parent;
            ImRect child;
            for (int frame = 0; frame < 4; ++frame) {
                ImGui::NewFrame();
                ImGui::Begin("SubmenuPlacementHost");
                if (frame == 0)
                    ImGui::OpenPopup("placement-root");
                ImGui::SetNextWindowPos({rightEdge ? 700.0F : 8.0F, y});
                REQUIRE(Ui::BeginMenuPopup("placement-root"));
                parent = {ImGui::GetWindowPos(),
                          ImVec2{ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), ImGui::GetWindowPos().y + ImGui::GetWindowHeight()}};
                if (frame == 0)
                    ImGui::OpenPopup("##submenu_popup_Scripts");
                REQUIRE(Ui::BeginContextSubmenu("Scripts", gui.fonts));
                for (int item = 0; item < 6; ++item) {
                    ImGui::PushID(item);
                    static_cast<void>(Ui::ContextMenuItem("A long localized script component name###script", nullptr, gui.fonts));
                    ImGui::PopID();
                }
                child = {ImGui::GetWindowPos(),
                         ImVec2{ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), ImGui::GetWindowPos().y + ImGui::GetWindowHeight()}};
                Ui::EndContextSubmenu();
                Ui::EndMenuPopup();
                ImGui::End();
                ImGui::Render();
                if (frame < 2)
                    continue;
                CHECK(child.Min.x >= 0);
                CHECK(child.Max.x <= 900);
                CHECK(child.Min.y >= 0);
                CHECK(child.Max.y <= 600);
                if (rightEdge)
                    CHECK(child.Max.x == Catch::Approx(parent.Min.x).margin(1));
                else
                    CHECK(child.Min.x == Catch::Approx(parent.Max.x).margin(1));
            }
        }
    }
}
