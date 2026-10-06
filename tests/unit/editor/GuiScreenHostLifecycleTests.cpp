#include "EditorActivityPackageSupport.h"
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
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "SecurityTestSupport.h"
#include "editor/project_model/RendererAvailability.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

    TEST_CASE("GUI inventory activation uses explicit artifact authority and revokes package surfaces on shutdown",
              "[unit][editor][Activity][ABI]") {
        Horo::Tests::EditorActivityPackage package;
        const auto installRoot = package.root.parent_path() / "horo107 GUI inventory";
        std::filesystem::remove_all(installRoot);

        struct RemoveInventory {
            std::filesystem::path root;

            ~RemoveInventory() {
                std::error_code error;
                std::filesystem::remove_all(root, error);
            }
        } removeInventory{installRoot};

        Extensions::ExtensionInventory inventory{installRoot};
        REQUIRE(inventory.InstallFromDirectory(package.root).HasValue());
        REQUIRE(inventory.SetEnabled("fixture.package", true).HasValue());
        REQUIRE(inventory.SetTrusted("fixture.package", true).HasValue());
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
        std::shared_ptr<const Security::NativeArtifactGate> gate;
        const bool admitted = GENERATE(false, true);
        if (admitted)
            gate = Horo::Tests::CreateAcceptingArtifactGate();
        GuiScreenHost host{gui,
                           modals,
                           settings,
                           localization,
                           engineEvents,
                           creation,
                           jobs,
                           input,
                           renderers,
                           ScreenRegistry{},
                           WorkspacePanelRegistry{},
                           0,
                           &inventory,
                           nullptr,
                           nullptr,
                           std::move(gate)};
        auto &activities = host.Services().Get<Extensions::EditorActivityHost>();
        activities.Update();
        const auto entry =
            std::ranges::find(inventory.Entries(), std::string{"fixture.package"}, &Extensions::ExtensionInventoryEntry::packageId);
        REQUIRE(entry != inventory.Entries().end());
        CHECK(entry->runtimeActive == admitted);
        if (admitted) {
            REQUIRE(activities.Prepared().size() == 1);
            const auto provider = activities.Prepared().front().surface.descriptor.provider;
            REQUIRE(activities.Registry().ToggleActivity(provider, "fixture.activity").HasValue());
            activities.Update();
            CHECK(activities.Prepared().front().surface.focused);
        } else {
            CHECK(activities.Prepared().empty());
            CHECK(entry->activationFailure.reason == Extensions::ExtensionActivationFailureReason::HostLoadFailed);
            CHECK_FALSE(entry->loadError.empty());
        }
        const auto &registry = activities.Registry();  // Host retains the activity authority until its destructor.
        host.Shutdown();
        CHECK(host.Services().Empty());
        CHECK(registry.Snapshot().empty());
        CHECK(registry.MoveActivity({"fixture.package", "fixture.module", 1}, "fixture.activity", {}).HasError());
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

        ShutdownAndCheckGuiScreenHost(host, stats, jobs);
    }

}  // namespace
