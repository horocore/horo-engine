#include "EditorActivityPackageSupport.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "SecurityTestSupport.h"
#include "editor/renderer/EditorGuiRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/ExtensionActivityView.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    class RecordingRenderer final : public IEditorGuiRenderer {
    public:
        std::size_t creates{};
        std::size_t destroys{};

        Result<void> Initialize() override {
            return Result<void>::Success();
        }

        Result<void> BeginFrame() override {
            return Result<void>::Success();
        }

        Result<void> RenderDrawData() override {
            return Result<void>::Success();
        }

        Result<std::uintptr_t> CreateTexture(const EditorRgba8ImageView &image) override {
            REQUIRE(image.IsValid());
            ++creates;
            return Result<std::uintptr_t>::Success(42);
        }

        void DestroyTexture(std::uintptr_t texture) noexcept override {
            if (texture == 42)
                ++destroys;
        }

        void Shutdown() noexcept override {}
    };

    struct Gui final {
        EngineDataBus engine;
        EditorDataBus editor;
        LocalizationService localization{LocaleTag{"tr-TR"}};
        Theme::Fonts fonts;
        ThemeContext theme{fonts};
        EditorSettingsSnapshot settings{.settings = DefaultEditorSettings()};
        EditorGuiContext context{engine, editor, localization, theme, settings};

        Gui() {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.DisplaySize = {640, 480};
            io.DeltaTime = 1.0F / 60.0F;
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.Fonts->AddFontDefault();
            static_cast<void>(io.Fonts->Build());
            auto *font = io.Fonts->Fonts.front();
            fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
            settings.settings.languageTag = "tr-TR";
        }

        ~Gui() {
            ImGui::DestroyContext();
        }
    };

    void CheckDrawerFocusAndNarrowFrames(ExtensionActivityView &view) {
        for (int frame = 0; frame < 3; ++frame) {
            ImGui::NewFrame();
            if (frame == 1) {
                REQUIRE(GImGui->NavWindow);
                CHECK(std::string_view{GImGui->NavWindow->Name} == "##ExtensionLeftDrawer");
            }
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({42, 480});
            ImGui::Begin("rail");
            CHECK_FALSE((ImGui::GetCurrentWindow()->Flags & ImGuiWindowFlags_NoNavInputs) != 0);
            ImGui::SetKeyboardFocusHere();
            static_cast<void>(view.DrawItems(false, 0, {1, 8}, 40, 470));
            CHECK(GImGui->LastItemData.ID != 0);
            CHECK_FALSE((GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0);
            ImGui::End();
            view.DrawDrawer(Extensions::EditorActivitySide::Left, {42, 0}, {80, 120});
            ImGui::Render();
            REQUIRE(ImGui::GetDrawData());
            CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
            view.Update();
        }
    }

    void CheckRailKeyboardFocus(ExtensionActivityView &view) {
        // ImGui applies focus API navigation results at the following NewFrame.
        ImGuiID railControl{};
        for (int phase = 0; phase < 2; ++phase) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({42, 480});
            ImGui::Begin("rail");
            if (phase == 0)
                ImGui::SetKeyboardFocusHere();
            static_cast<void>(view.DrawItems(false, 0, {1, 8}, 40, 470));
            if (phase == 0)
                railControl = GImGui->LastItemData.ID;
            else {
                CHECK(GImGui->NavId == railControl);
                CHECK(GImGui->NavWindow == ImGui::GetCurrentWindow());
                CHECK(GImGui->LastItemData.ID == railControl);
            }
            ImGui::End();
            ImGui::Render();
        }
    }

    void CheckPlacementAndStaleMove(ExtensionActivityView &view, const std::shared_ptr<Extensions::EditorActivityHost> &host,
                                    const Extensions::EditorSurfaceProviderIdentity &provider, EditorWorkspaceController &workspace,
                                    const std::string &leftPanel) {
        REQUIRE(view.QueueMove(provider, "fixture.activity", {Extensions::EditorActivitySide::Right, 1, 0}));
        CHECK(view.Count(false, 0) == 1);  // Draw-side command only queues; Update owns publication.
        view.Update();
        CHECK(view.Count(true, 1) == 1);
        CHECK(view.HasDrawer(Extensions::EditorActivitySide::Right));
        REQUIRE(view.TakeNativePanelClear() == Extensions::EditorActivitySide::Right);
        REQUIRE(view.QueueMove(provider, "fixture.activity", {Extensions::EditorActivitySide::Bottom, 2, 0}));
        REQUIRE(host->Registry().PublishActivity(provider, "fixture.activity", {true, true, 3}).HasValue());
        host->Update();
        view.Update();  // An intervening revision invalidates the queued drag.
        CHECK(view.Count(true, 1) == 1);
        CHECK_FALSE(view.TakeNativePanelClear().has_value());
        REQUIRE(view.QueueMove(provider, "fixture.activity", {Extensions::EditorActivitySide::Bottom, 2, 0}));
        view.Update();
        const auto destination = view.TakeNativePanelClear();
        REQUIRE(destination == Extensions::EditorActivitySide::Bottom);
        workspace.ProcessCommand({.command = EditorWorkspaceViewCommand::ChangeActivePanel,
                                  .targetIndex = static_cast<int>(*destination),
                                  .stringPayload = std::string{}});
        CHECK(workspace.ViewModel().activeBottomPanelId.empty());
        CHECK(workspace.ViewModel().activeLeftPanelId == leftPanel);
        CHECK(view.HasDrawer(Extensions::EditorActivitySide::Bottom));
    }

    void CheckBottomMouseActivation(ExtensionActivityView &view, const std::shared_ptr<Extensions::EditorActivityHost> &host,
                                    EditorWorkspaceController &workspace, const std::string &leftPanel) {
        REQUIRE(host->Registry().Close("fixture.activity").HasValue());
        host->Update();
        view.Update();
        workspace.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::ChangeActivePanel, .targetIndex = 2, .stringPayload = "horo.global_dock"});
        REQUIRE_FALSE(workspace.ViewModel().activeBottomPanelId.empty());
        bool clicked{};
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(15, 20);
        for (int step = 0; step < 3; ++step) {
            if (step > 0)
                io.AddMouseButtonEvent(0, step == 1);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({42, 480});
            ImGui::Begin("rail");
            clicked = view.DrawItems(false, 2, {1, 8}, 40, 470) || clicked;
            ImGui::End();
            ImGui::Render();
        }
        REQUIRE(clicked);
        const auto clickedDestination = view.TakeNativePanelClear();
        REQUIRE(clickedDestination == Extensions::EditorActivitySide::Bottom);
        workspace.ProcessCommand({.command = EditorWorkspaceViewCommand::ChangeActivePanel,
                                  .targetIndex = static_cast<int>(*clickedDestination),
                                  .stringPayload = std::string{}});
        CHECK(workspace.ViewModel().activeBottomPanelId.empty());
        CHECK(workspace.ViewModel().activeLeftPanelId == leftPanel);
    }

    TEST_CASE("Actual activity GUI retains uploaded SVG textures across frames and withdraws them after provider retirement",
              "[unit][editor][Activity][ABI]") {
        Horo::Tests::EditorActivityPackage package;
        JobSystem jobs;
        auto host = std::make_shared<Extensions::EditorActivityHost>(jobs);
        Extensions::ExtensionManager manager{nullptr,
                                             Extensions::ExtensionHostProfile::Interactive,
                                             {HORO_EDITOR_ACTIVITY_HOST_CAPABILITY},
                                             Horo::Tests::CreateAcceptingArtifactGate(),
                                             {},
                                             {},
                                             host};
        REQUIRE(manager.LoadExtension(package.root.string()).HasValue());
        host->Update();
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource runtimeCancellation;
        REQUIRE(runtimeScene.Startup(runtimeCancellation.Token()).HasValue());
        EditorWorkspaceController workspace{package.root, runtimeScene};
        const auto leftPanel = workspace.ViewModel().activeLeftPanelId;
        REQUIRE_FALSE(leftPanel.empty());
        REQUIRE_FALSE(workspace.ViewModel().activeBottomPanelId.empty());
        Gui gui;
        RecordingRenderer renderer;
        ExtensionActivityView view{gui.context, host.get(), &renderer};
        view.Update();
        CHECK(renderer.creates == 1);
        CHECK(view.Count(false, 0) == 1);
        const auto provider = host->Prepared().front().surface.descriptor.provider;
        REQUIRE(host->Registry().ToggleActivity(provider, "fixture.activity").HasValue());
        host->Update();
        view.Update();
        REQUIRE(view.HasDrawer(Extensions::EditorActivitySide::Left));
        REQUIRE(view.TakeNativePanelClear() == Extensions::EditorActivitySide::Left);
        CheckDrawerFocusAndNarrowFrames(view);
        CheckRailKeyboardFocus(view);
        CHECK(renderer.creates == 1);
        CHECK(renderer.destroys == 0);
        CheckPlacementAndStaleMove(view, host, provider, workspace, leftPanel);
        CheckBottomMouseActivation(view, host, workspace, leftPanel);
        host->Update();
        view.Update();
        static_cast<void>(view.TakeNativePanelClear());
        REQUIRE(view.QueueMove(provider, "fixture.activity", {Extensions::EditorActivitySide::Left, 0, 0}));
        manager.UnloadExtension("fixture.package");
        CHECK_FALSE(view.HasDrawer(Extensions::EditorActivitySide::Left));
        host->Update();
        view.Update();
        CHECK(view.Count(false, 0) == 0);
        CHECK(renderer.destroys == 1);
        CHECK_FALSE(view.TakeNativePanelClear().has_value());
        CHECK(renderer.creates == 1);
        CHECK_FALSE(view.QueueMove(provider, "fixture.activity", {Extensions::EditorActivitySide::Left, 0, 0}));
    }
}  // namespace
