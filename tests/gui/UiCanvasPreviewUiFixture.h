#pragma once

#include "EditorUiTestHarness.h"
#include "Horo/Editor/DefaultWorkspacePanels.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/GuiScreen.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "UiStyleWorkspaceFixture.h"
#include "editor/input/EditorInputActions.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <imgui_internal.h>
#include <imgui_test_engine/imgui_te_context.h>
#include <imgui_test_engine/imgui_te_engine.h>
#include <iterator>

namespace Horo::Tests::CanvasUi {
    using namespace Editor;
    using namespace Runtime::Ui;

    /** @brief The existing Test Engine owns ImGui; actual view/controller own rendering and canonical edits. */
    struct Fixture final {
        EditorUiTestHarness harness{CreateHeadlessEditorUiTestSurface()};
        UiStyleTest::Project project;
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtime;
        Input::InputRouter input;
        Input::InputContextToken inputContext;
        LocalizationService localization;
        EngineDataBus engine;
        EditorSettingsSnapshot settings;
        Theme::Fonts fonts{Fonts()};
        ThemeContext theme{fonts};
        WorkspacePanelRegistry panels;
        std::unique_ptr<EditorWorkspaceController> controller;
        std::unique_ptr<EditorGuiContext> gui;
        std::unique_ptr<EditorWorkspaceView> view;
        ImVec2 extent{1280, 900};
        std::size_t authoredCommands{};

        explicit Fixture(const char *locale, const bool overlappingPeer = false) : localization{Editor::LocaleTag{locale}} {
            SetExtent(extent);
            PrepareResources(locale);
            WriteCanvas(overlappingPeer);
            REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
            controller =
                std::make_unique<EditorWorkspaceController>(project.root, runtime, project.assets.Snapshot(),
                                                            EditorWorkspaceDependencies{.mutableAssetRegistry = &project.assets,
                                                                                        .uiInspectorResources = project.resources.get(),
                                                                                        .mutations = &project.mutations,
                                                                                        .durableFiles = &project.files,
                                                                                        .jobs = &project.jobs,
                                                                                        .documentSessionOwner = project.workspace});
            const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation.Token()};
            REQUIRE(runtime.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
            controller->SynchronizeRuntimeScenePreview();
            RegisterDefaultWorkspacePanels(panels);
            REQUIRE(input.SetActionMap(BuildEditorInputActions()).HasValue());
            inputContext = input.PushContext(Input::InputContextId{kEditorWorkspaceInputContext}, Input::InputContextKind::EditorWorkspace);
            gui = std::make_unique<EditorGuiContext>(EditorGuiContext{engine, controller->DataBus(), localization, theme, settings});
            view = std::make_unique<EditorWorkspaceView>(*gui, panels, 0, input, inputContext);
            EditorWorkspaceViewCommandData open;
            open.command = EditorWorkspaceViewCommand::OpenSourceFile;
            open.sourceOpenRequest = SourceOpenRequest{.path = "Hud.uicanvas"};
            controller->ProcessCommand(open);
            controller->UpdateUiCanvasResources();
            REQUIRE(controller->ViewModel().uiCanvasDocuments.size() == 1);
            REQUIRE(Source().preview.layout.has_value());
        }

        ~Fixture() {
            view.reset();
            controller.reset();
            runtime.Shutdown();
        }

        Fixture(const Fixture &) = delete;
        Fixture &operator=(const Fixture &) = delete;

        static Theme::Fonts Fonts() {
            auto *font = ImGui::GetIO().FontDefault;
            return {.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
        }

        void PrepareResources(const char *locale) {
            project.style.style.assignments.clear();
            project.style.style.states.clear();
            project.WriteStyle();
            project.Rebuild();
            project.Prepare();
            REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
            REQUIRE(localization.Prepare(Editor::LocaleTag{locale}));
            REQUIRE(localization.ActivatePrepared());
        }

        void WriteCanvas(const bool overlappingPeer) {
            UiDocumentBuilder builder{project.Identity<UiDocumentId>(11), UiDocumentRevision::Create(1).Value()};
            const auto root = project.Identity<UiElementId>(13);
            REQUIRE(builder
                        .AddCanvas({.id = project.Identity<UiCanvasId>(12),
                                    .rootElement = root,
                                    .referenceResolution = {800, 600},
                                    .presentation = {.safeArea = UiSafeAreaMode::Inset}})
                        .HasValue());
            REQUIRE(builder.AddElement(CreateUiElement(UiElementKind::Canvas, root, {}).Value()).HasValue());
            auto panel = CreateUiElement(UiElementKind::Panel, project.Identity<UiElementId>(14), root).Value();
            panel.presentation->layout.positioning = UiLayoutPositioning::Absolute;
            panel.presentation->layout.width = UiLength::Percent(64);
            panel.presentation->layout.height = UiLength::Percent(64);
            REQUIRE(builder.AddElement(panel).HasValue());
            if (overlappingPeer) {
                auto peer = panel;
                peer.id = project.Identity<UiElementId>(16);
                REQUIRE(builder.AddElement(peer).HasValue());
            }
            auto compact = panel.presentation->layout;
            compact.width = UiLength::Percent(32);
            REQUIRE(builder
                        .AddPresentationProfiles({project.Identity<UiCanvasId>(12),
                                                  UiPresentationProfileRevision::Create(1).Value(),
                                                  {{project.Identity<UiPresentationProfileId>(15),
                                                    1,
                                                    {.formFactor = UiPresentationFormFactor::Handheld},
                                                    {{panel.id, compact}}}}})
                        .HasValue());
            auto source = SerializeUiDocument(std::move(builder).Build().Value());
            REQUIRE(source.HasValue());
            std::ofstream(project.root / "Hud.uicanvas")
                << nlohmann::json{{"format", "horo.ui_canvas"}, {"formatVersion", 2}, {"document", nlohmann::json::parse(source.Value())}};
        }

        void SetExtent(ImVec2 value) {
            extent = value;
            // The existing null surface leaves IO untouched; set this before the next harness NewFrame.
            ImGui::GetIO().DisplaySize = extent;
        }

        void LogPresentation(ImGuiTestContext &ui) {
            const auto &io = ImGui::GetIO();
            const auto *viewport = ImGui::GetMainViewport();
            const auto &model = controller->ViewModel();
            ui.LogInfo("Surface=%s IO=%.1f,%.1f viewport=%.1f,%.1f work=%.1f,%.1f content=%.1f,%.1f active=%llu canvas=%llu",
                       harness.Surface().RendererName().data(), io.DisplaySize.x, io.DisplaySize.y, viewport->Size.x, viewport->Size.y,
                       viewport->WorkSize.x, viewport->WorkSize.y, extent.x, extent.y,
                       static_cast<unsigned long long>(
                           model.workspacePanelHost.ActiveDocument() ? model.workspacePanelHost.ActiveDocument()->Value() : 0),
                       static_cast<unsigned long long>(Source().identity.instance.Value()));
            ui.LogInfo("Dock left=%s right=%s bottom=%s widths=%.1f,%.1f bottomHeight=%.1f", model.activeLeftPanelId.c_str(),
                       model.activeRightPanelId.c_str(), model.activeBottomPanelId.c_str(), model.leftPanelWidth, model.rightPanelWidth,
                       model.bottomPanelHeight);
            for (const auto *window : ImGui::GetCurrentContext()->Windows)
                if (window->WasActive)
                    ui.LogInfo("Window %s size=%.1f,%.1f content=%.1f,%.1f work=(%.1f,%.1f)-(%.1f,%.1f)", window->Name, window->Size.x,
                               window->Size.y, window->ContentSize.x, window->ContentSize.y, window->WorkRect.Min.x, window->WorkRect.Min.y,
                               window->WorkRect.Max.x, window->WorkRect.Max.y);
        }

        void Frame(ImGuiTestContext *) {
            controller->UpdateUiCanvasResources();
            EditorWorkspaceViewCommandData command;
            view->Draw(controller->ViewModel(), command, {.width = extent.x, .height = extent.y});
            if (command.command == EditorWorkspaceViewCommand::UiCanvasCommitLayout)
                ++authoredCommands;
            if (command.command != EditorWorkspaceViewCommand::None)
                controller->ProcessCommand(command);
        }

        const UiCanvasWorkspaceDocument &Source() const {
            return controller->ViewModel().uiCanvasDocuments.front();
        }

        UiLayoutStyle Layout() const {
            return Source().selectedElement->presentation->layout;
        }

        std::string SavedBytes() const {
            std::ifstream stream(project.root / "Hud.uicanvas", std::ios::binary);
            return {std::istreambuf_iterator<char>{stream}, {}};
        }

        std::string Label(const char *key) const {
            return localization.Get("editor", std::string{"workspace.ui_canvas."} + key);
        }
    };

    /** @brief Scrolls the existing narrow document to its real toolbar before an offscreen control is targeted. */
    inline bool RevealToolbar(ImGuiTestContext &ui, const Fixture &fixture) {
        if (fixture.extent.x != 640)
            return true;
        const auto content = ui.WindowInfo("##DockDocument/##DockContent");
        IM_CHECK_RETV(content.Window != nullptr, false);
        ui.ScrollToTop(content.Window->ID);
        ui.Yield(2);
        return !ui.IsError();
    }

    /** @brief Clicks a real localized control; each command is consumed by the actual GUI frame. */
    inline void Click(ImGuiTestContext &ui, const Fixture &fixture, const char *key) {
        if (!RevealToolbar(ui, fixture))
            return;
        ui.ItemClick(("//**/" + fixture.Label(key)).c_str());
        ui.Yield(2);
    }

    /** @brief Opens the existing shared combo by stable identity and chooses its actual localized popup row. */
    inline bool Choose(ImGuiTestContext &ui, const Fixture &fixture, const char *id, const char *key, const int option) {
        if (!RevealToolbar(ui, fixture))
            return false;
        ui.ItemClick((std::string{"//**/"} + id + "/Combo###" + id).c_str());
        if (ui.IsError())
            return false;
        ui.ItemClick(("//**/" + fixture.Label(key) + "###combo_option_" + std::to_string(option)).c_str());
        ui.Yield(2);
        return !ui.IsError();
    }

    /** @brief Reveal the actual child item through Test Engine so inspector growth cannot scroll past the preview. */
    inline bool RevealPreview(ImGuiTestContext &ui, const Fixture &fixture) {
        if (fixture.extent.x != 640)
            return true;
        const auto content = ui.WindowInfo("##DockDocument/##DockContent");
        IM_CHECK_RETV(content.Window != nullptr, false);
        const auto &windows = ImGui::GetCurrentContext()->Windows;
        const auto child = std::find_if(windows.begin(), windows.end(), [&](const ImGuiWindow *window) {
            return window->ParentWindow == content.Window &&
                   std::string_view{window->Name}.find("/canvas_preview_") != std::string_view::npos;
        });
        IM_CHECK_RETV(child != windows.end(), false);
        // Window pointers remain stable when Test Engine yields and focus reorders ImGui's window vector.
        const auto *preview = *child;
        // Long toolbar navigation may scroll both axes; reveal the document origin through the public input operation.
        ui.ScrollToX(content.Window->ID, 0);
        if (ui.IsError())
            return false;
        // Use current observed window geometry; this non-navigable child need not emit a parent item ID.
        const float target = content.Window->Scroll.y + preview->Pos.y - content.Window->InnerRect.Min.y -
                             (content.Window->InnerRect.GetHeight() - preview->Size.y) * 0.5F;
        ui.LogInfo("Preview scroll before parent=(%.1f,%.1f)-(%.1f,%.1f) scroll=%.1f max=%.1f child=(%.1f,%.1f) size=%.1f,%.1f active=%d "
                   "clipped=(%.1f,%.1f)-(%.1f,%.1f)",
                   content.Window->InnerRect.Min.x, content.Window->InnerRect.Min.y, content.Window->InnerRect.Max.x,
                   content.Window->InnerRect.Max.y, content.Window->Scroll.y, content.Window->ScrollMax.y, preview->Pos.x, preview->Pos.y,
                   preview->Size.x, preview->Size.y, preview->Active, preview->ClipRect.Min.x, preview->ClipRect.Min.y,
                   preview->ClipRect.Max.x, preview->ClipRect.Max.y);
        ui.ScrollToY(content.Window->ID, target);
        ui.Yield(2);
        ui.LogInfo("Preview scroll after scroll=%.1f max=%.1f child=(%.1f,%.1f) active=%d skip=%d clipped=(%.1f,%.1f)-(%.1f,%.1f)",
                   content.Window->Scroll.y, content.Window->ScrollMax.y, preview->Pos.x, preview->Pos.y, preview->Active,
                   preview->SkipItems, preview->ClipRect.Min.x, preview->ClipRect.Min.y, preview->ClipRect.Max.x, preview->ClipRect.Max.y);
        return !ui.IsError();
    }

    /** @brief Targets copied runtime bounds inside the actual Test Engine item; expected authored values never use this transform. */
    inline ImVec2 Center(ImGuiTestContext &ui, const Fixture &fixture) {
        ui.LogInfo("Canonical preview present=%d diagnostic=%s", fixture.Source().preview.layout.has_value(),
                   fixture.Source().preview.diagnostic ? fixture.Source().preview.diagnostic->message.c_str() : "none");
        IM_CHECK_RETV(fixture.Source().preview.layout.has_value(), ImVec2());
        if (!RevealPreview(ui, fixture))
            return {};
        const auto item = ui.ItemInfo("//**/canvas_interaction");
        IM_CHECK_RETV(item.ID != 0, ImVec2());
        IM_CHECK_RETV(item.RectClipped.GetWidth() > 0 && item.RectClipped.GetHeight() > 0, ImVec2());
        const auto &preview = *fixture.Source().preview.layout;
        const auto &state = fixture.Source().preview.state;
        const auto box = std::ranges::find(preview.boxes, fixture.project.Identity<UiElementId>(14), &UiCanvasPreviewBox::element);
        IM_CHECK_RETV(box != preview.boxes.end(), ImVec2());
        const float padding = Theme::GetActiveTokens().spacing.propertyRowGap * 2;
        const auto size = item.RectFull.GetSize();
        const float deviceScale = std::max(0.001F, std::min((size.x - padding * 2) / preview.viewport.pixelExtent.width,
                                                            (size.y - padding * 2) / preview.viewport.pixelExtent.height)) *
                                  static_cast<float>(state.zoomPercent) / 100;
        const auto safe = preview.canvas.ContentPixelRect();
        ui.LogInfo("Canvas viewport=%u,%u logical=%d,%d safe=%u,%u,%u,%u zoom=%u pan=%d,%d device=%u rotated=%d",
                   preview.viewport.pixelExtent.width, preview.viewport.pixelExtent.height, preview.canvas.logicalExtent.width,
                   preview.canvas.logicalExtent.height, safe.x, safe.y, safe.width, safe.height, state.zoomPercent, state.pan.x,
                   state.pan.y, static_cast<unsigned>(state.device), state.rotated);
        const float scale = deviceScale * safe.width / preview.canvas.logicalExtent.width;
        const ImVec2 origin{item.RectFull.Min.x + padding + state.pan.x * scale + safe.x * deviceScale,
                            item.RectFull.Min.y + padding + state.pan.y * scale + safe.y * deviceScale};
        const ImVec2 low{std::max(item.RectClipped.Min.x + 2, origin.x + box->bounds.origin.x * scale),
                         std::max(item.RectClipped.Min.y + 2, origin.y + box->bounds.origin.y * scale)};
        const ImVec2 high{std::min(item.RectClipped.Max.x - 2, origin.x + (box->bounds.origin.x + box->bounds.extent.width) * scale),
                          std::min(item.RectClipped.Max.y - 2, origin.y + (box->bounds.origin.y + box->bounds.extent.height) * scale)};
        ui.LogInfo("Canvas target full=(%.2f,%.2f)-(%.2f,%.2f) clipped=(%.2f,%.2f)-(%.2f,%.2f) scale=%.8f origin=(%.2f,%.2f)",
                   item.RectFull.Min.x, item.RectFull.Min.y, item.RectFull.Max.x, item.RectFull.Max.y, item.RectClipped.Min.x,
                   item.RectClipped.Min.y, item.RectClipped.Max.x, item.RectClipped.Max.y, scale, origin.x, origin.y);
        ui.LogInfo("Canvas box origin=(%d,%d) extent=(%d,%d) target=(%.2f,%.2f)-(%.2f,%.2f)", box->bounds.origin.x, box->bounds.origin.y,
                   box->bounds.extent.width, box->bounds.extent.height, low.x, low.y, high.x, high.y);
        IM_CHECK_RETV(high.x > low.x && high.y > low.y, ImVec2());
        return {(low.x + high.x) * 0.5F, (low.y + high.y) * 0.5F};
    }

    /** @brief Sends actual mouse edges through the existing input bridge and production preview handlers. */
    inline void Drag(ImGuiTestContext &ui, const ImVec2 start, const ImVec2 delta, const ImGuiMouseButton button = 0) {
        if (ui.IsError())
            return;
        ui.MouseMoveToPos(start);
        ui.MouseDown(button);
        ui.Yield();
        ui.MouseMoveToPos({start.x + delta.x, start.y + delta.y});
        ui.Yield();
        ui.MouseUp(button);
        ui.Yield(2);
    }
}  // namespace Horo::Tests::CanvasUi
