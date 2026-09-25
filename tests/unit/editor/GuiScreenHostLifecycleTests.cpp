#include "../../helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "editor/project_model/RendererAvailability.h"
#include "editor/ui_preview/EditorUiPreviewCatalog.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>
#include <memory>

namespace Horo::Editor::Theme {
    struct Fonts;
}

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct ScreenStats {
        int enters = 0;
        int leaves = 0;
        int destructions = 0;
    };

    class RecordingScreen final : public GuiScreen {
    public:
        explicit RecordingScreen(ScreenStats &stats) : stats_(stats) {}

        ~RecordingScreen() override {
            ++stats_.destructions;
        }

        [[nodiscard]] ScreenId Id() const override {
            return 1;
        }

        [[nodiscard]] Result<void> OnEnter(const GuiRoute &) override {
            ++stats_.enters;
            return Result<void>::Success();
        }

        void OnUpdate(float) override {}

        void Draw(const GuiContentRegion &) override {}

        [[nodiscard]] LeaveDecision CanLeave(const LeaveTarget &) const override {
            return {.disposition = LeaveDisposition::Allow, .requirement = std::nullopt};
        }

        [[nodiscard]] Result<LeaveDecision> ResolveLeave(const LeaveTarget &, const LeaveResolution &) override {
            return Result<LeaveDecision>::Success({.disposition = LeaveDisposition::Allow, .requirement = std::nullopt});
        }

        void OnLeave() override {
            ++stats_.leaves;
        }

    private:
        ScreenStats &stats_;
    };

    void ShutdownAndCheckGuiScreenHost(GuiScreenHost &host, ScreenStats &stats, JobSystem &jobs) {
        host.Shutdown();
        REQUIRE((host.IsShutdown()));
        REQUIRE((stats.leaves == 1));
        REQUIRE((stats.destructions == 1));
        REQUIRE((host.Services().Empty()));

        host.Shutdown();
        REQUIRE((stats.leaves == 1));
        REQUIRE((stats.destructions == 1));
        const Result<void> navigation = host.Navigate(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}});
        REQUIRE((navigation.HasError()));
        REQUIRE((navigation.ErrorValue().domain.Value() == "horo.editor.screens"));
        REQUIRE((navigation.ErrorValue().code.Value() == "navigation.host_shutdown"));
        jobs.Shutdown(ShutdownPolicy::Cancel);
    }

    void ExerciseUiPreviewScenarios(GuiScreenHost &host, EditorModalHost &modals, Horo::Editor::Tests::HeadlessEditorGuiFixture &imgui) {
        host.DispatchMenuInvocation(EditorMenuInvocation{.action = EditorMenuAction::ImportAssets});
        REQUIRE(modals.HasOpenModal());
        REQUIRE(host.StartUiPreview("asset-import-empty").HasError());
        const auto menuModalId = modals.TopModalId();
        REQUIRE(menuModalId.has_value());
        REQUIRE(modals.RequestClose(*menuModalId, ModalCloseReason::Cancelled).HasValue());
        modals.OnUpdate(0.016F);
        REQUIRE_FALSE(modals.HasOpenModal());

        const auto invalid = host.StartUiPreview("not-a-preview");
        REQUIRE(invalid.HasError());
        REQUIRE(host.StartUiPreview("asset-import-empty").HasValue());
        const auto duplicate = host.StartUiPreview("asset-import-empty");
        REQUIRE(duplicate.HasError());
        CHECK(duplicate.ErrorValue().code.Value() == "navigation.host_already_started");

        imgui.BeginFrame();
        host.Draw();
        imgui.EndFrame();

        const auto previewModalId = modals.TopModalId();
        REQUIRE(previewModalId.has_value());
        REQUIRE(modals.RequestClose(*previewModalId, ModalCloseReason::Cancelled).HasValue());
        modals.OnUpdate(0.016F);
        REQUIRE_FALSE(modals.HasOpenModal());

        const ImGuiWindow *const gallery = ImGui::FindWindowByName("##EditorUiPreviewGallery");
        REQUIRE(gallery != nullptr);
        const float firstButtonTop = EditorUiPreviewHeaderHeight + 54.0F;
        const float buttonHeight = 38.0F;
        const float secondButtonCenter = firstButtonTop + buttonHeight + ImGui::GetStyle().ItemSpacing.y + buttonHeight * 0.5F;
        ImGuiIO &io = ImGui::GetIO();
        io.AddMousePosEvent(gallery->Pos.x + EditorUiPreviewSidebarWidth * 0.5F, gallery->Pos.y + secondButtonCenter);
        imgui.BeginFrame();
        host.Draw();
        imgui.EndFrame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        imgui.BeginFrame();
        host.Draw();
        imgui.EndFrame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        imgui.BeginFrame();
        host.Draw();
        imgui.EndFrame();
        REQUIRE(modals.HasOpenModal());
    }

    TEST_CASE("Gui Screen Host Registers Core Status And Shuts Down Safely", "[unit][editor]") {
        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        Input::InputRouter input;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
        ProjectCreationService creation{jobs, engineEvents};
        LocalizationService localization{LocaleTag{"en-US"}};
        ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings());
        EditorSettingsService settings{DefaultEditorSettings(), configuration, editorEvents, localization};
        EditorModalHost modals{editorEvents, input};
        const Theme::Fonts &fonts = *reinterpret_cast<const Theme::Fonts *>(static_cast<std::uintptr_t>(1));
        ThemeContext theme{fonts};
        EditorSettingsSnapshot settingsSnapshot = settings.Snapshot();
        EditorGuiContext gui{engineEvents, editorEvents, localization, theme, settingsSnapshot};
        RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}},
                                               "opengl"};
        ScreenStats stats;
        ScreenRegistry screens;
        screens.Register(GuiRouteKind::Welcome, [](const EditorServiceRegistry &services, const GuiRoute &) {
            return std::make_unique<RecordingScreen>(services.Get<ScreenStats>());
        });
        WorkspacePanelRegistry panels;

        GuiScreenHost host{gui,  modals, settings,  localization,       engineEvents,     creation,
                           jobs, input,  renderers, std::move(screens), std::move(panels)};
        REQUIRE((host.StatusItems().Find("horo.status.backend") != nullptr));
        REQUIRE((host.StatusItems().Find("horo.status.cpu") == nullptr));
        REQUIRE((&host.Services().Get<JobSystem>() == &jobs));
        REQUIRE((stats.enters == 0));
        REQUIRE((host.Navigate(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}).HasError()));
        host.Services().Register(stats);
        REQUIRE((host.Start(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}).HasValue()));
        REQUIRE((stats.enters == 1));
        REQUIRE((!host.Services().Empty()));

        const Result<void> invalidRoute = host.Navigate(GuiRoute{GuiRouteKind::Welcome, ProjectCreationRouteParameters{}});
        REQUIRE((invalidRoute.HasError()));
        REQUIRE((invalidRoute.ErrorValue().domain.Value() == "horo.editor.screens"));
        REQUIRE((invalidRoute.ErrorValue().code.Value() == "navigation.invalid_route_parameters"));

        ShutdownAndCheckGuiScreenHost(host, stats, jobs);
    }

    TEST_CASE("Gui Screen Host admits only known isolated UI preview scenarios", "[unit][editor][gui]") {
        ::Horo::Editor::Tests::HeadlessEditorGuiFixture imgui;
        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        Input::InputRouter input;
        ::Horo::Editor::Tests::ScopedJobSystem jobs;
        ProjectCreationService creation{jobs.Get(), engineEvents};
        LocalizationService localization{LocaleTag{"en-US"}};
        ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings());
        EditorSettingsService settings{DefaultEditorSettings(), configuration, editorEvents, localization};
        EditorModalHost modals{editorEvents, input};
        const Theme::Fonts &fonts = imgui.Fonts();
        ThemeContext theme{fonts};
        EditorGuiContext gui{engineEvents, editorEvents, localization, theme, settings.Snapshot()};
        RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}},
                                               "opengl"};

        GuiScreenHost host{gui,
                           modals,
                           settings,
                           localization,
                           engineEvents,
                           creation,
                           jobs.Get(),
                           input,
                           renderers,
                           ScreenRegistry{},
                           WorkspacePanelRegistry{}};
        ExerciseUiPreviewScenarios(host, modals, imgui);

        host.Shutdown();
        CHECK(host.IsShutdown());
        CHECK(host.StartUiPreview("asset-import-empty").HasError());
    }
}  // namespace
