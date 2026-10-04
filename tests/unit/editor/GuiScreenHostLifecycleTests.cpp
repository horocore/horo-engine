#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "editor/project_model/RendererAvailability.h"
#include "editor/update/UpdateExperienceSession.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>

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

    /** @brief Holds discovery until settings closes, then counts host-owned preparation and activation. */
    class BackgroundUpdateBackend final : public IEditorUpdateBackend {
    public:
        std::atomic<bool> checkStarted{false};
        std::atomic<bool> releaseCheck{false};
        std::atomic<unsigned> preparations{0U};
        unsigned activations{};

        Result<std::optional<EditorUpdateOffer>> Check(const EditorUpdateChannel &, const CancellationToken cancellation) override {
            checkStarted = true;
            while (!releaseCheck && !cancellation.IsCancellationRequested())
                std::this_thread::yield();
            if (cancellation.IsCancellationRequested())
                return Result<std::optional<EditorUpdateOffer>>::Failure(JobCancelled().ErrorValue());
            return Result<std::optional<EditorUpdateOffer>>::Success(EditorUpdateOffer{"0.4.2", {}, {}, true});
        }

        Result<void> Prepare(const EditorUpdateOffer &, CancellationToken,
                             const std::function<void(EditorUpdatePhase, std::uint64_t, std::uint64_t)> &) override {
            ++preparations;
            return Result<void>::Success();
        }

        Result<void> Activate(CancellationToken) override {
            ++activations;
            return Result<void>::Success();
        }

        Result<void> Rollback(CancellationToken) override {
            return Result<void>::Success();
        }
    };

    /** @brief Uses host updates only after actual modal closure; no draw or direct session polling is involved. */
    void CheckUpdatesAfterSettingsClose(GuiScreenHost &host, EditorModalHost &modals, EditorGuiContext &gui,
                                        EditorSettingsService &settings, JobSystem &jobs) {
        BackgroundUpdateBackend backend;
        UpdateExperienceSession session{jobs, backend};

        struct UpdateBinding final {
            EditorGuiContext &gui;
            UpdateExperienceSession *previous;

            explicit UpdateBinding(EditorGuiContext &context) : gui(context), previous(context.updates) {}

            UpdateBinding(const UpdateBinding &) = delete;
            UpdateBinding &operator=(const UpdateBinding &) = delete;
            UpdateBinding(UpdateBinding &&) = delete;
            UpdateBinding &operator=(UpdateBinding &&) = delete;

            ~UpdateBinding() {
                gui.updates = previous;
            }
        } binding{gui};

        gui.updates = &session;
        session.SetAutomaticDownload(true);
        session.SetInstallOnExit(true);
        REQUIRE(modals.OpenRoot(std::make_unique<SettingsModal>(gui, settings, 0)).HasValue());
        modals.OnUpdate(0.0F);
        REQUIRE(session.CheckNow());
        const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!backend.checkStarted && std::chrono::steady_clock::now() < startDeadline)
            std::this_thread::yield();
        REQUIRE(backend.checkStarted);
        REQUIRE(modals.RequestClose(ModalId{SettingsModal::kModalId}, ModalCloseReason::Cancelled).HasValue());
        modals.OnUpdate(0.0F);
        REQUIRE_FALSE(modals.HasOpenModal());
        CHECK(session.Snapshot().canCancel);
        backend.releaseCheck = true;
        const auto completionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (session.Snapshot().phase != EditorUpdatePhase::RestartRequired && std::chrono::steady_clock::now() < completionDeadline) {
            host.OnUpdate(0.0F);
            std::this_thread::yield();
        }
        REQUIRE(session.Snapshot().phase == EditorUpdatePhase::RestartRequired);
        CHECK(backend.preparations == 1U);
        host.OnUpdate(0.0F);
        CHECK(backend.preparations == 1U);
        const auto handoff = session.ActivateOnExit();
        REQUIRE(handoff.HasValue());
        CHECK(handoff.Value());
        CHECK(backend.activations == 1U);
        CHECK_FALSE(session.ActivateOnExit().Value());
        CHECK(backend.activations == 1U);
    }

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

        SECTION("Closed settings does not stop discovery, automatic download, or install-on-exit") {
            CheckUpdatesAfterSettingsClose(host, modals, gui, settings, jobs);
        }
        ShutdownAndCheckGuiScreenHost(host, stats, jobs);
    }

}  // namespace
