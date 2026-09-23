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
#include <string>

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
        const float inspectorWidth = defaultFont->CalcTextSizeA(12.0F, 100000.0F, 0.0F, tabs.front()).x + 20.0F;
        const float sceneWidth = defaultFont->CalcTextSizeA(12.0F, 100000.0F, 0.0F, tabs.back()).x + 20.0F;
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
    const ComponentSizeMetrics &xs = MetricsFor(entry.designTokens, ComponentSize::XS);
    REQUIRE(xs.fontSize == 14.0F);
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
