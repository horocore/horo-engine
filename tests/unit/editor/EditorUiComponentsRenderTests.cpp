#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Debug/ConsoleConfiguration.h"
#include "Horo/Debug/ConsoleVariableSet.h"
#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorSnackbarHost.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeConsoleRecordList.h"
#include "RuntimeConsoleView.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
    /** @brief Composes the same canonical console schema as the actual Editor root. */
    Horo::ConfigurationSchema ConsoleViewSchema() {
        auto descriptors = Horo::Debug::ConsoleConfigurationDescriptors(Horo::Debug::ProductProfile::Editor);
        REQUIRE(descriptors.HasValue());
        Horo::ConfigurationSchema schema;
        for (const auto &descriptor : descriptors.Value())
            REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        return schema;
    }

    /** @brief Counts queries over the real bounded log authority. */
    struct ConsoleObservedLogs final : Horo::Log::IStructuredLogQuery {
        Horo::Log::StructuredLogStore store{512};
        mutable std::size_t queries{};

        std::optional<Horo::Log::StructuredLogSnapshot> SnapshotIfChanged(std::uint64_t revision) const override {
            ++queries;
            return store.SnapshotIfChanged(revision);
        }
    };

    /** @brief Counts reads of the actual immutable application network query, without an alternate producer/store. */
    struct ConsoleObservedNetwork final : Horo::Application::INetworkDebuggerQuery {
        std::shared_ptr<Horo::Application::NetworkDebuggerService> service{
            std::make_shared<Horo::Application::NetworkDebuggerService>(std::chrono::hours{1})};
        mutable std::size_t queries{};

        Horo::Application::NetworkDebuggerProjection Query() const override {
            ++queries;
            return service->Query();
        }
    };

    /** @brief Counts reads of the same operation authority used by the executor. */
    struct ConsoleObservedOperations final : Horo::IOperationQuery {
        const Horo::OperationStore &store;
        mutable std::size_t queries{};

        explicit ConsoleObservedOperations(const Horo::OperationStore &value) : store(value) {}

        std::optional<Horo::OperationStoreSnapshot> SnapshotIfChanged(std::uint64_t revision) const override {
            ++queries;
            return store.SnapshotIfChanged(revision);
        }
    };

    /** @brief Real core/configuration/input/operation authorities; only native window/font realization is omitted. */
    struct ConsoleViewFixture final {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0}};
        std::shared_ptr<Horo::Debug::ConsoleHistory> history{std::make_shared<Horo::Debug::ConsoleHistory>()};
        std::shared_ptr<Horo::OperationStore> operations{std::make_shared<Horo::OperationStore>(8, 8)};
        ConsoleObservedOperations operationQuery{*operations};
        ConsoleObservedLogs logs;
        Horo::Input::InputService input;
        std::shared_ptr<Horo::ConfigurationService> configurationOwner{std::make_shared<Horo::ConfigurationService>(ConsoleViewSchema())};
        Horo::ConfigurationService &configuration{*configurationOwner};
        std::shared_ptr<Horo::Debug::CommandRegistry> registry;
        std::shared_ptr<Horo::Debug::ConsoleVariableSet> variables;
        std::unique_ptr<Horo::Debug::CommandExecutor> executor;
        std::unique_ptr<Horo::Editor::RuntimeConsoleView> view;

        explicit ConsoleViewFixture(const bool terminal = true, const bool overlays = false,
                                    std::span<const Horo::Debug::OverlaySource> panels = {},
                                    Horo::Debug::DebugCommandContext principal = {3, false, false}, const bool sceneInspection = false,
                                    std::shared_ptr<const Horo::Application::INetworkDebuggerQuery> network = {}) {
            Horo::Debug::RegistryPolicy policy;
            policy.profile = Horo::Debug::ProductProfile::Editor;
            auto created = Horo::Debug::CommandRegistry::Create(Horo::Debug::CoreConsoleDescriptors(), policy);
            REQUIRE(created.HasValue());
            registry = std::make_shared<Horo::Debug::CommandRegistry>(std::move(created).Value());
            std::vector descriptors{Horo::Debug::ConsoleHistoryVariableDescriptor(), Horo::Debug::ConsoleVisibilityVariableDescriptor(),
                                    Horo::Debug::ConsoleLogOverlayVariableDescriptor(),
                                    Horo::Debug::ConsoleJobsOverlayVariableDescriptor()};
            if (sceneInspection)
                descriptors.push_back(Horo::Debug::ConsoleSceneOverlayVariableDescriptor());
            if (network)
                descriptors.push_back(Horo::Debug::ConsoleNetworkOverlayVariableDescriptor());
            auto inventory =
                Horo::Debug::ConsoleVariableSet::Create(descriptors, Horo::Debug::CoreConsoleDescriptors(), policy, configurationOwner);
            REQUIRE(inventory.HasValue());
            variables = std::make_shared<Horo::Debug::ConsoleVariableSet>(std::move(inventory).Value());
            REQUIRE(variables->Set(Horo::Debug::ConsoleVisibleVariableName, terminal, {}).HasValue());
            for (const auto source : {Horo::Debug::OverlaySource::Logs, Horo::Debug::OverlaySource::Jobs}) {
                const auto name = source == Horo::Debug::OverlaySource::Logs ? Horo::Debug::ConsoleLogOverlayVariableName
                                                                             : Horo::Debug::ConsoleJobsOverlayVariableName;
                REQUIRE(variables->Set(name, std::ranges::find(panels, source) != panels.end(), {3, false, false}).HasValue());
            }
            if (network)
                REQUIRE(variables
                            ->Set(Horo::Debug::ConsoleNetworkOverlayVariableName,
                                  std::ranges::find(panels, Horo::Debug::OverlaySource::Network) != panels.end(), {3, false, false})
                            .HasValue());
            auto bound = Horo::Debug::BindCoreConsoleCommands(registry, history, "Product Editor", variables);
            if (sceneInspection) {
                REQUIRE(variables
                            ->Set(Horo::Debug::ConsoleSceneOverlayVariableName,
                                  std::ranges::find(panels, Horo::Debug::OverlaySource::Scene) != panels.end(), {3, false, false})
                            .HasValue());
                REQUIRE(variables->Apply(Horo::Debug::ConsoleVariableApplyPolicy::NextFrame).HasValue());
            }
            REQUIRE(bound.HasValue());
            auto dispatched = Horo::Debug::CommandExecutor::Create(registry, bound.Value(), jobs, operations, std::this_thread::get_id(),
                                                                   std::this_thread::get_id(), 128, nullptr, variables);
            REQUIRE(dispatched.HasValue());
            executor = std::move(dispatched).Value();
            input.BeginFrame(1);
            static_cast<void>(input.CommitFrame());
            view =
                std::make_unique<Horo::Editor::RuntimeConsoleView>(Horo::Editor::RuntimeConsoleViewPorts{input.Router(), *executor,
                                                                                                         *history, *registry,
                                                                                                         operationQuery, configuration,
                                                                                                         logs, principal, variables.get(),
                                                                                                         sceneInspection, network},
                                                                   Horo::Editor::RuntimeConsoleViewPresentation{terminal, overlays,
                                                                                                                panels});
        }
    };

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

TEST_CASE("Shared command input retains native completion and Enter submission semantics", "[editor][gui][console][design-system]") {
    using namespace Horo::Editor::Ui;
    ImGuiTestContext imgui{{640, 400}};
    std::array<char, 64> value{};
    std::string completion = "version";
    InputTextNavigation navigation{&completion,
                                   [](void *context, InputTextNavigationAction action, std::string_view current,
                                      std::size_t cursor) -> std::optional<std::string_view> {
        if (action == InputTextNavigationAction::Complete && cursor == current.size())
            return *static_cast<std::string *>(context);
        if (action == InputTextNavigationAction::Previous && current.size() >= 3)
            return current.substr(0, 3);
        return {};
    }};
    bool focus = true;
    bool submitted{};
    const auto draw = [&] {
        ImGui::SetNextWindowSize({500, 250});
        ImGui::Begin("CommandInputGallery");
        if (focus) {
            ImGui::SetKeyboardFocusHere();
            focus = false;
        }
        submitted = InputTextControl("##command", value.data(), value.size(), imgui.fonts,
                                     {.componentSize = ComponentSize::Medium, .submitOnEnter = true, .navigation = &navigation});
        ImGui::End();
    };
    RenderImGuiFrame(draw);
    RenderImGuiFrame(draw);
    imgui.io->AddInputCharactersUTF8("ver");
    RenderImGuiFrame(draw);
    CHECK(std::string_view(value.data()) == "ver");
    CHECK_FALSE(submitted);
    imgui.io->AddKeyEvent(ImGuiKey_Tab, true);
    RenderImGuiFrame(draw);
    CHECK(std::string_view(value.data()) == "version");
    imgui.io->AddKeyEvent(ImGuiKey_Tab, false);
    RenderImGuiFrame(draw);
    CHECK(std::string_view(value.data()) == "version");
    CHECK_FALSE(submitted);
    imgui.io->AddKeyEvent(ImGuiKey_UpArrow, true);
    RenderImGuiFrame(draw);
    CHECK(std::string_view(value.data()) == "ver");
    imgui.io->AddKeyEvent(ImGuiKey_UpArrow, false);
    RenderImGuiFrame(draw);
    CHECK(std::string_view(value.data()) == "ver");
    imgui.io->AddKeyEvent(ImGuiKey_Tab, true);
    RenderImGuiFrame(draw);
    imgui.io->AddKeyEvent(ImGuiKey_Tab, false);
    RenderImGuiFrame(draw);
    CHECK(std::string_view(value.data()) == "version");
    imgui.io->AddKeyEvent(ImGuiKey_Enter, true);
    RenderImGuiFrame(draw);
    CHECK(submitted);
    imgui.io->AddKeyEvent(ImGuiKey_Enter, false);
    RenderImGuiFrame(draw);
    CHECK_FALSE(submitted);
}

TEST_CASE("Runtime console record gallery clips long Unicode rows and bounds painting across a full history",
          "[editor][gui][console][design-system]") {
    using namespace Horo::Editor;
    Horo::Debug::ConsoleLimits limits;
    limits.historyEntries = 10000;
    Horo::Debug::ConsoleHistory history(limits);
    const std::string command = "find \"" + std::string(128, 'x') + "\xc3\xb6\"";
    for (std::size_t index = 0; index < 10000; ++index)
        history.Append({Horo::Debug::FindCommandId, {}, command}, "Tamamlandi\nAyrintili sonuc");
    REQUIRE(history.Entries().size() == 10000);
    auto revision = history.Revision();
    ImGuiTestContext imgui{{640, 400}};
    for (const bool enabled : {true, false}) {
        Ui::RuntimeConsoleRecordListResult painted;
        const auto draw = [&] {
            ImGui::SetNextWindowPos({20, 20});
            ImGui::SetNextWindowSize({260, 300});
            ImGui::Begin("RuntimeConsoleRecordGallery");
            painted = Ui::RuntimeConsoleRecordList({.rows = history.Entries(),
                                                    .revision = history.Revision(),
                                                    .empty = "Bos",
                                                    .pending = "Bekleniyor",
                                                    .unavailable = "Artik saklanmiyor",
                                                    .selected = history.Entries().size() - 1,
                                                    .height = 170,
                                                    .enabled = enabled},
                                                   imgui.fonts);
            ImGui::End();
        };
        RenderImGuiFrame(draw);
        RenderImGuiFrame(draw);
        CHECK(painted.painted > 0);
        CHECK(painted.painted <= 128);
        CHECK(painted.painted < history.Entries().size());
        CHECK(history.Revision() == revision);
        CHECK_FALSE(painted.activated);
        bool checkedChild{};
        ImVec2 rowTarget{};
        for (const auto *window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && std::string_view(window->Name).find("##RuntimeConsoleRecords") != std::string_view::npos) {
                CHECK(window->ScrollMax.x == 0);
                CHECK(window->ScrollMax.y > 0);
                CHECK(window->Scroll.y == Catch::Approx(window->ScrollMax.y).margin(1));
                rowTarget = window->InnerRect.GetCenter();
                checkedChild = true;
            }
        }
        CHECK(checkedChild);
        if (enabled && checkedChild) {
            imgui.io->AddMousePosEvent(rowTarget.x, rowTarget.y);
            RenderImGuiFrame(draw);
            imgui.io->AddMouseButtonEvent(0, true);
            RenderImGuiFrame(draw);
            history.Append({Horo::Debug::FindCommandId, {}, "find changed"}, "changed");
            revision = history.Revision();
            imgui.io->AddMouseButtonEvent(0, false);
            RenderImGuiFrame(draw);
            CHECK_FALSE(painted.activated);
        }
    }
}

TEST_CASE("Runtime console view does not steal existing or newer modal leases and releases input on host barriers",
          "[editor][console][input]") {
    ConsoleViewFixture console;
    auto gameplay = console.input.Router().PushContext(Horo::Input::InputContextId("gameplay"), Horo::Input::InputContextKind::Gameplay);
    auto modal =
        console.input.Router().PushContext(Horo::Input::InputContextId("existing.modal"), Horo::Input::InputContextKind::ModalRoot);
    console.view->SynchronizeFocus(false);
    CHECK_FALSE(console.view->Interactive());
    CHECK(console.input.Router().IsContextActive(modal));
    modal.Reset();
    console.view->SynchronizeFocus(false);
    CHECK(console.view->Interactive());
    CHECK_FALSE(console.input.Router().IsContextActive(gameplay));
    auto child = console.input.Router().PushContext(Horo::Input::InputContextId("later.modal"), Horo::Input::InputContextKind::ModalRoot);
    console.view->SynchronizeFocus(false);
    CHECK_FALSE(console.view->Interactive());
    CHECK(console.input.Router().IsContextActive(child));
    child.Reset();
    console.view->SynchronizeFocus(false);
    CHECK(console.view->Interactive());
    console.view->SynchronizeFocus(true);
    CHECK_FALSE(console.view->Interactive());
    console.input.BeginFrame(2);
    static_cast<void>(console.input.CommitFrame());
    CHECK(console.input.Router().IsContextActive(gameplay));
    CHECK(console.view->Visible());
    console.view->SetVisible(false);
    CHECK_FALSE(console.view->Visible());
    CHECK(console.history->Entries().empty());
}

TEST_CASE("Runtime log and operation panels use real visible sources and explicit host refresh time", "[editor][gui][console][overlays]") {
    using namespace Horo::Editor;
    ImGuiTestContext imgui{{1400, 900}};
    const std::array requested{Horo::Debug::OverlaySource::Logs, Horo::Debug::OverlaySource::Jobs, Horo::Debug::OverlaySource::Frame};
    ConsoleViewFixture console(false, true, requested);
    console.logs.store.Append({0, {}, Horo::Log::Level::Warn, "game.scene", "visible source", {}});
    REQUIRE(console.operations->Begin({.title = "real operation", .progress = 0.5F}));
    LocalizationService localization{{"tr-TR"}};
    REQUIRE(localization.LoadCatalogFile(std::filesystem::path(HORO_PROJECT_SOURCE_DIR) / "assets/localization/editor/tr-TR.json"));
    REQUIRE(localization.Prepare({"tr-TR"}));
    REQUIRE(localization.ActivatePrepared());
    auto now = std::chrono::steady_clock::time_point{};
    bool blocked = true;
    const auto draw = [&] {
        console.view->Draw(localization, imgui.fonts, blocked, now);
    };
    RenderImGuiFrame(draw);
    CHECK(console.logs.queries == 0);
    CHECK(console.operationQuery.queries == 0);
    blocked = false;
    RenderImGuiFrame(draw);
    RenderImGuiFrame(draw);
    CHECK(console.view->Interactive());
    CHECK_FALSE(console.view->Visible());
    CHECK(console.view->OverlaysVisible());
    CHECK(console.logs.queries == 1);
    CHECK(console.operationQuery.queries == 1);
    bool logWindow{}, operationWindow{};
    for (const auto *window : ImGui::GetCurrentContext()->Windows) {
        if (!window->Active)
            continue;
        logWindow |= std::string_view(window->Name) == "##RuntimeLogsOverlay";
        operationWindow |= std::string_view(window->Name) == "##RuntimeJobsOverlay";
    }
    CHECK(logWindow);
    CHECK(operationWindow);
    now += std::chrono::milliseconds{249};
    RenderImGuiFrame(draw);
    CHECK(console.logs.queries == 1);
    CHECK(console.operationQuery.queries == 1);
    blocked = true;
    now += std::chrono::seconds{1};
    RenderImGuiFrame(draw);
    CHECK_FALSE(console.view->Interactive());
    CHECK(console.logs.queries == 1);
    CHECK(console.operationQuery.queries == 1);
    blocked = false;
    RenderImGuiFrame(draw);
    CHECK(console.logs.queries == 2);
    CHECK(console.operationQuery.queries == 2);
    console.view->SetOverlaysVisible(false);
    now += std::chrono::seconds{10};
    RenderImGuiFrame(draw);
    CHECK_FALSE(console.view->AnyVisible());
    CHECK(console.logs.queries == 2);
    CHECK(console.operationQuery.queries == 2);
}

TEST_CASE("Accepted panel variables project to real source queries under the independent overlay feature gate",
          "[editor][gui][console][overlays]") {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Debug;
    ImGuiTestContext imgui{{1400, 900}};
    const std::array requested{OverlaySource::Logs, OverlaySource::Jobs};
    ConsoleViewFixture console(false, true, requested);
    console.logs.store.Append({0, {}, Log::Level::Info, "game", "real log", {}});
    REQUIRE(console.operations->Begin({.title = "real operation"}));
    LocalizationService localization{LocaleTag{"en-US"}};
    REQUIRE(localization.LoadCatalogFile(std::filesystem::path(HORO_PROJECT_SOURCE_DIR) / "assets/localization/editor/en-US.json"));
    REQUIRE(localization.Prepare({"en-US"}));
    REQUIRE(localization.ActivatePrepared());
    auto now = std::chrono::steady_clock::time_point{};
    const auto draw = [&] {
        console.view->Draw(localization, imgui.fonts, false, now);
    };
    const auto project = [&] {
        for (const auto source : {OverlaySource::Logs, OverlaySource::Jobs}) {
            const auto name = source == OverlaySource::Logs ? ConsoleLogOverlayVariableName : ConsoleJobsOverlayVariableName;
            const auto value = console.variables->Read(name, {3, false, false});
            REQUIRE(value.HasValue());
            REQUIRE(console.view->SetPanelVisibility(source, std::get<bool>(value.Value().active)).HasValue());
        }
        CHECK_FALSE(console.view->TakePanelVisibilityRequest());
    };
    RenderImGuiFrame(draw);
    RenderImGuiFrame(draw);
    CHECK(console.logs.queries == 1);
    CHECK(console.operationQuery.queries == 1);
    const auto revision = console.configuration.Snapshot().Revision();
    REQUIRE(console.executor->Submit("overlay.logs.visible false", {3, false, false}).HasValue());
    REQUIRE(console.executor->DrainOwner(1).HasValue());
    project();
    now += std::chrono::seconds{1};
    RenderImGuiFrame(draw);
    CHECK(console.logs.queries == 1);
    CHECK(console.operationQuery.queries == 2);
    console.view->SetOverlaysVisible(false);
    REQUIRE(console.executor->Submit("overlay.logs.visible true", {3, false, false}).HasValue());
    REQUIRE(console.executor->DrainOwner(1).HasValue());
    project();
    now += std::chrono::seconds{1};
    RenderImGuiFrame(draw);
    CHECK(console.logs.queries == 1);
    CHECK(console.operationQuery.queries == 2);
    console.view->SetOverlaysVisible(true);
    RenderImGuiFrame(draw);
    CHECK(console.logs.queries == 2);
    CHECK(console.operationQuery.queries == 3);
    CHECK(console.configuration.Snapshot().Revision() == revision);
    CHECK(console.view->SetPanelVisibility(OverlaySource::Frame, true).HasError());
}

TEST_CASE("Public terminal presentation cannot enable developer store panels", "[editor][console][overlays]") {
    const std::array requested{Horo::Debug::OverlaySource::Logs, Horo::Debug::OverlaySource::Jobs};
    ConsoleViewFixture console(false, true, requested, {1, false, false});
    console.view->SynchronizeFocus(false);
    CHECK_FALSE(console.view->AnyVisible());
    CHECK_FALSE(console.view->Interactive());
    CHECK(console.logs.queries == 0);
    CHECK(console.operationQuery.queries == 0);
    CHECK(console.view->SetPanelVisibility(Horo::Debug::OverlaySource::Logs, false).HasError());
}

TEST_CASE("Graphical variable completion uses the same dispatcher inventory and host permissions", "[editor][gui][console]") {
    using namespace Horo::Editor;
    for (const bool developer : {false, true}) {
        ImGuiTestContext imgui{{1000, 700}};
        ConsoleViewFixture console(true, false, {}, {developer ? 3U : 1U, false, false});
        LocalizationService localization{LocaleTag{"en-US"}};
        REQUIRE(localization.LoadCatalogFile(std::filesystem::path(HORO_PROJECT_SOURCE_DIR) / "assets/localization/editor/en-US.json"));
        REQUIRE(localization.Prepare({"en-US"}));
        REQUIRE(localization.ActivatePrepared());
        const auto draw = [&] {
            console.view->Draw(localization, imgui.fonts, false, std::chrono::steady_clock::time_point{});
        };
        RenderImGuiFrame(draw);
        RenderImGuiFrame(draw);
        imgui.io->AddInputCharactersUTF8("console.h");
        RenderImGuiFrame(draw);
        imgui.io->AddKeyEvent(ImGuiKey_Tab, true);
        RenderImGuiFrame(draw);
        imgui.io->AddKeyEvent(ImGuiKey_Tab, false);
        RenderImGuiFrame(draw);
        imgui.io->AddKeyEvent(ImGuiKey_Enter, true);
        RenderImGuiFrame(draw);
        imgui.io->AddKeyEvent(ImGuiKey_Enter, false);
        RenderImGuiFrame(draw);
        const auto operations = console.operations->SnapshotIfChanged(0);
        if (developer) {
            REQUIRE(console.history->Entries().size() == 1);
            CHECK(console.history->Entries()[0].command == "console.history.limit");
            REQUIRE(operations);
            REQUIRE(operations->operations.size() == 1);
            CHECK(console.history->Entries()[0].operation == operations->operations[0].id);
            REQUIRE(console.executor->DrainOwner(1).HasValue());
        } else {
            CHECK(console.history->Entries().empty());
            CHECK_FALSE(operations);
        }
    }
}

TEST_CASE("Scene development panel requires an explicit owner capability and follows accepted visibility",
          "[editor][gui][console][scene]") {
    using namespace Horo::Editor;
    ImGuiTestContext imgui{{1200, 800}};
    const std::array requested{Horo::Debug::OverlaySource::Scene};
    ConsoleViewFixture console(false, true, requested, {3, false, false}, true);
    Horo::Runtime::RuntimeSceneService scene(Horo::Runtime::SceneRuntimeId{101});
    REQUIRE(scene.Startup({}).HasValue());
    Horo::Runtime::SceneDefinitionBuilder builder({1}, {1});
    builder.Add(Horo::Runtime::RuntimeEntityDefinition{.object = {1}});
    auto definition = std::move(builder).Build();
    REQUIRE(definition.HasValue());
    REQUIRE(scene.QueuePreparation(std::move(definition).Value()).HasValue());
    const Horo::Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, {}};
    REQUIRE(scene.OnPhase(Horo::Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
    const auto sceneView = scene.ActiveScene();
    REQUIRE(sceneView);
    REQUIRE(console.view->PublishScene(&*sceneView).HasValue());
    LocalizationService localization{{"en-US"}};
    REQUIRE(localization.LoadCatalogFile(std::filesystem::path(HORO_PROJECT_SOURCE_DIR) / "assets/localization/editor/en-US.json"));
    REQUIRE(localization.Prepare({"en-US"}));
    REQUIRE(localization.ActivatePrepared());
    auto now = std::chrono::steady_clock::time_point{};
    const auto draw = [&] {
        console.view->Draw(localization, imgui.fonts, false, now);
    };
    RenderImGuiFrame(draw);
    RenderImGuiFrame(draw);
    const auto *window = ImGui::FindWindowByName("##RuntimeSceneOverlay");
    REQUIRE(window);
    CHECK(window->Active);
    CHECK(console.logs.queries == 0);
    CHECK(console.operationQuery.queries == 0);
    REQUIRE(console.view->SetPanelVisibility(Horo::Debug::OverlaySource::Scene, false).HasValue());
    RenderImGuiFrame(draw);
    CHECK_FALSE(window->Active);
    scene.Shutdown();
    REQUIRE(console.view->PublishScene(&*sceneView).HasValue());
    REQUIRE(console.view->SetPanelVisibility(Horo::Debug::OverlaySource::Scene, true).HasValue());
    now += std::chrono::seconds{1};
    RenderImGuiFrame(draw);
    CHECK(window->Active);
    REQUIRE(console.view->PublishScene(nullptr).HasValue());
    RenderImGuiFrame(draw);
    CHECK(console.logs.queries == 0);
    CHECK(console.operationQuery.queries == 0);
    ConsoleViewFixture unbound(false, true, requested);
    CHECK(unbound.view->PublishScene(&*sceneView).HasError());
    CHECK(unbound.view->SetPanelVisibility(Horo::Debug::OverlaySource::Scene, true).HasError());
}

TEST_CASE("Network panel uses the actual query only while visible and keeps unrelated sources idle", "[editor][gui][console][network]") {
    using namespace Horo::Editor;
    for (const auto locale : {"en-US", "tr-TR"}) {
        ImGuiTestContext imgui{{800, 700}};
        auto network = std::make_shared<ConsoleObservedNetwork>();
        const std::array requested{Horo::Debug::OverlaySource::Network};
        ConsoleViewFixture console(false, true, requested, {3, false, false}, false, network);
        CHECK(network->queries == 0);
        CHECK_FALSE(network->service->Query().snapshot.attached);
        LocalizationService localization{LocaleTag{locale}};
        REQUIRE(localization.LoadCatalogFile(std::filesystem::path(HORO_PROJECT_SOURCE_DIR) / "assets/localization/editor" /
                                             (std::string(locale) + ".json")));
        REQUIRE(localization.Prepare(LocaleTag{locale}));
        REQUIRE(localization.ActivatePrepared());
        auto now = std::chrono::steady_clock::time_point{};
        bool blocked = true;
        const auto draw = [&] {
            console.view->Draw(localization, imgui.fonts, blocked, now);
        };
        RenderImGuiFrame(draw);
        CHECK(network->queries == 0);
        blocked = false;
        RenderImGuiFrame(draw);
        RenderImGuiFrame(draw);
        const auto *window = ImGui::FindWindowByName("##RuntimeNetworkOverlay");
        REQUIRE(window);
        CHECK(window->Active);
        CHECK(network->queries == 1);
        CHECK(console.logs.queries == 0);
        CHECK(console.operationQuery.queries == 0);
        now += std::chrono::milliseconds{249};
        RenderImGuiFrame(draw);
        CHECK(network->queries == 1);
        now += std::chrono::milliseconds{1};
        RenderImGuiFrame(draw);
        CHECK(network->queries == 2);
        REQUIRE(console.view->SetPanelVisibility(Horo::Debug::OverlaySource::Network, false).HasValue());
        now += std::chrono::seconds{1};
        RenderImGuiFrame(draw);
        CHECK_FALSE(window->Active);
        CHECK(network->queries == 2);
        console.view->SetOverlaysVisible(false);
        REQUIRE(console.view->SetPanelVisibility(Horo::Debug::OverlaySource::Network, true).HasValue());
        RenderImGuiFrame(draw);
        CHECK(network->queries == 2);
        console.view->SetOverlaysVisible(true);
        RenderImGuiFrame(draw);
        CHECK(window->Active);
        CHECK(network->queries == 3);
        CHECK(console.logs.queries == 0);
        CHECK(console.operationQuery.queries == 0);
        ConsoleViewFixture unbound(false, true, requested);
        CHECK(unbound.view->SetPanelVisibility(Horo::Debug::OverlaySource::Network, true).HasError());
    }
}

TEST_CASE("Inspection identity rows use bounded shared virtual painting", "[editor][gui][console][scene]") {
    using namespace Horo::Editor;
    ImGuiTestContext imgui{{1000, 700}};
    std::array<Ui::RuntimeInspectionRecordRow, 256> rows;
    for (auto &row : rows) {
        const std::string_view reference{"101:0:1"};
        std::copy(reference.begin(), reference.end(), row.title.begin());
        row.titleBytes = static_cast<std::uint8_t>(reference.size());
    }
    Ui::RuntimeConsoleRecordListResult painted;
    const auto draw = [&] {
        ImGui::SetNextWindowSize({600, 500}, ImGuiCond_Always);
        ImGui::Begin("SceneInspectionGallery");
        painted = Ui::RuntimeInspectionRecordList({rows, 1, "empty", "Parent", {}, 400}, imgui.fonts);
        ImGui::End();
    };
    RenderImGuiFrame(draw);
    RenderImGuiFrame(draw);
    CHECK(painted.painted > 0);
    CHECK(painted.painted <= 128);
}

TEST_CASE("Graphical runtime console Tab and Enter use the same core dispatcher and redacted history", "[editor][gui][console]") {
    using namespace Horo::Editor;
    ImGuiTestContext imgui{{1000, 700}};
    ConsoleViewFixture console;
    LocalizationService localization{LocaleTag{"en-US"}};
    REQUIRE(localization.LoadCatalogFile(std::filesystem::path(HORO_PROJECT_SOURCE_DIR) / "assets/localization/editor/en-US.json"));
    REQUIRE(localization.Prepare({"en-US"}));
    REQUIRE(localization.ActivatePrepared());
    const auto draw = [&] {
        console.view->Draw(localization, imgui.fonts, false, std::chrono::steady_clock::time_point{});
    };
    RenderImGuiFrame(draw);
    RenderImGuiFrame(draw);
    REQUIRE(console.view->Interactive());
    imgui.io->AddInputCharactersUTF8("ver");
    RenderImGuiFrame(draw);
    imgui.io->AddKeyEvent(ImGuiKey_Tab, true);
    RenderImGuiFrame(draw);
    imgui.io->AddKeyEvent(ImGuiKey_Tab, false);
    RenderImGuiFrame(draw);
    imgui.io->AddKeyEvent(ImGuiKey_Enter, true);
    RenderImGuiFrame(draw);
    imgui.io->AddKeyEvent(ImGuiKey_Enter, false);
    RenderImGuiFrame(draw);
    REQUIRE(console.history->Entries().size() == 1);
    CHECK(console.history->Entries()[0].command == "version");
    CHECK(console.history->Entries()[0].output == "Product Editor");
    CHECK_FALSE(console.operations->SnapshotIfChanged(0));
    imgui.io->AddKeyEvent(ImGuiKey_Escape, true);
    RenderImGuiFrame(draw);
    const auto close = console.view->TakeVisibilityRequest();
    REQUIRE(close);
    CHECK_FALSE(*close);
    CHECK_FALSE(console.view->TakeVisibilityRequest());
    CHECK(console.view->Visible());
    CHECK(console.view->Interactive());
    REQUIRE(console.executor->Submit("console.visible false", {}).HasValue());
    REQUIRE(console.executor->DrainOwner(1).HasValue());
    const auto visible = console.variables->Read(Horo::Debug::ConsoleVisibleVariableName, {});
    REQUIRE(visible.HasValue());
    console.view->SetVisible(std::get<bool>(visible.Value().active));
    CHECK_FALSE(console.view->Visible());
    CHECK_FALSE(console.view->Interactive());
}
