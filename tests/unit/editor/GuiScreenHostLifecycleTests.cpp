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

#include <catch2/catch_test_macros.hpp>
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
        int menuInvocations = 0;
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

        bool HandleMenuInvocation(const EditorMenuInvocation &) override {
            ++stats_.menuInvocations;
            return true;
        }

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

    void VerifyMenuInputBarrier(GuiScreenHost &host, Input::InputRouter &input, const ScreenStats &stats) {
        host.DispatchMenuInvocation(EditorMenuInvocation{.action = EditorMenuAction::SaveScene});
        REQUIRE(stats.menuInvocations == 1);
        auto modalContext = input.PushContext(Input::InputContextId{"test.modal"}, Input::InputContextKind::ModalRoot);
        host.DispatchMenuInvocation(EditorMenuInvocation{.action = EditorMenuAction::SaveScene});
        REQUIRE(stats.menuInvocations == 1);
        modalContext.Reset();
        host.DispatchMenuInvocation(EditorMenuInvocation{.action = EditorMenuAction::SaveScene});
        REQUIRE(stats.menuInvocations == 1);
        const Input::RawInputSnapshot nextFrame;
        input.BeginFrame(nextFrame);
        host.DispatchMenuInvocation(EditorMenuInvocation{.action = EditorMenuAction::SaveScene});
        REQUIRE(stats.menuInvocations == 2);
    }

    TEST_CASE("Shutdown Leaves Once Destroys Screen And Revokes Services", "[unit][editor]") {
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
        REQUIRE((&host.Services().Get<JobSystem>() == &jobs));
        REQUIRE((stats.enters == 0));
        REQUIRE((host.Navigate(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}).HasError()));
        host.Services().Register(stats);
        REQUIRE((host.Start(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}).HasValue()));
        REQUIRE((stats.enters == 1));
        REQUIRE((!host.Services().Empty()));

        VerifyMenuInputBarrier(host, input, stats);

        const Result<void> invalidRoute = host.Navigate(GuiRoute{GuiRouteKind::Welcome, ProjectCreationRouteParameters{}});
        REQUIRE((invalidRoute.HasError()));
        REQUIRE((invalidRoute.ErrorValue().domain.Value() == "horo.editor.screens"));
        REQUIRE((invalidRoute.ErrorValue().code.Value() == "navigation.invalid_route_parameters"));

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
}  // namespace
