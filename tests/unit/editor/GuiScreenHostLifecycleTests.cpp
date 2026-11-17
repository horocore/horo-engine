#include "EditorActivityPackageSupport.h"
#include "EditorInspectorTestSupport.h"
#include "GameplayRetirementTestSupport.h"
#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorMenuModel.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/EditorPageHost.h"
#include "Horo/Extensions/EditorThemeTokens.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Gameplay/GameModuleRetirement.h"
#include "Horo/Navigation/NavigationSettingsErrors.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "SecurityTestSupport.h"
#include "editor/modals/build_release/BuildModal.h"
#include "editor/modals/build_release/BuildReleaseModal.h"
#include "editor/project_model/RendererAvailability.h"
#include "editor/update/UpdateExperienceSession.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <future>
#include <imgui.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace Horo::Editor::Theme {
    struct Fonts;
}

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    enum class FixedAction {
        None,
        Close,
        Shutdown,
        Throw,
        Navigate
    };

    struct ScreenStats {
        int enters = 0;
        int leaves = 0;
        int destructions = 0;
        GuiScreenHost *closeFromUpdate{};
        bool closeCallbackStayedAlive{};
        std::optional<Error> navigationFailure;
        GuiScreenHost *fixedHost{};
        Runtime::RuntimeDispatchSource source;
        FixedAction fixedAction{};
        int fixedUpdates{}, fixedPreparations{}, fixedCommits{}, fixedAborts{};
        bool fixedCallbackStayedAlive{}, committedProof{};
        bool denyLeave{};
    };

    class RecordingScreen final : public GuiScreen {
    public:
        explicit RecordingScreen(ScreenStats &stats) : GuiScreen("test.RecordingScreen"), stats_(stats) {}

        RecordingScreen(const RecordingScreen &) = delete;
        RecordingScreen &operator=(const RecordingScreen &) = delete;
        RecordingScreen(RecordingScreen &&) = delete;
        RecordingScreen &operator=(RecordingScreen &&) = delete;

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

        void OnUpdate(float) override {
            if (stats_.closeFromUpdate) {
                auto &stats = stats_;
                auto *host = std::exchange(stats.closeFromUpdate, nullptr);
                (void)host->RequestCloseApplication();
                stats.closeCallbackStayedAlive = stats.leaves == 0 && stats.destructions == 0;
            }
        }

        void Draw(const GuiContentRegion &) override {
            // This shutdown fixture intentionally submits no graphical controls.
        }

        Result<void> OnRuntimeFixedUpdate(const Runtime::FixedStepContext &) override {
            ++stats_.fixedUpdates;
            switch (stats_.fixedAction) {
                case FixedAction::Close:
                    static_cast<void>(stats_.fixedHost->RequestCloseApplication());
                    break;
                case FixedAction::Shutdown:
                    stats_.fixedHost->Shutdown();
                    break;
                case FixedAction::Throw:
                    throw std::runtime_error("Injected screen fixed failure.");
                case FixedAction::Navigate:
                    static_cast<void>(stats_.fixedHost->Navigate({GuiRouteKind::ProjectCreation, ProjectCreationRouteParameters{}}));
                    break;
                case FixedAction::None:
                    break;
            }
            stats_.fixedCallbackStayedAlive = stats_.leaves == 0 && stats_.destructions == 0;
            return Result<void>::Success();
        }

        Result<void> PrepareRuntimeFixedCommit(const Runtime::FixedStepContext &) override {
            ++stats_.fixedPreparations;
            return Result<void>::Success();
        }

        void CommitRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            ++stats_.fixedCommits;
            Runtime::RuntimeDispatchFacts facts;
            stats_.committedProof = context.dispatchEvidence.Read(stats_.source, Runtime::RuntimePhase::FixedUpdate, facts) ==
                                        Runtime::RuntimeDispatchStatus::Valid &&
                                    facts.committedTick == context.simulationTick && facts.committedAttempt == context.attemptNumber;
        }

        void AbortRuntimeFixedUpdate(const Runtime::FixedStepContext &) noexcept override {
            ++stats_.fixedAborts;
        }

        [[nodiscard]] Result<NavigationProjectSettingsSession *> NavigationSettings() override {
            if (stats_.navigationFailure)
                return Result<NavigationProjectSettingsSession *>::Failure(*stats_.navigationFailure);
            return Result<NavigationProjectSettingsSession *>::Success(nullptr);
        }

        [[nodiscard]] LeaveDecision CanLeave(const LeaveTarget &) const override {
            return {.disposition = stats_.denyLeave ? LeaveDisposition::Deny : LeaveDisposition::Allow, .requirement = std::nullopt};
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

    class GuiFixedParticipant final : public Runtime::RuntimeLifecycleParticipant {
    public:
        explicit GuiFixedParticipant(GuiScreenHost &host) : host_(host) {}

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {}

        Result<void> OnPhase(Runtime::RuntimePhase, const Runtime::FrameContext &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &c) override {
            auto forged = c;
            ++forged.simulationTick;
            forgedRejected = host_.OnRuntimeFixedUpdate(forged).HasError();
            evidence = c.dispatchEvidence;
            return host_.OnRuntimeFixedUpdate(c);
        }

        Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &c) override {
            return host_.PrepareRuntimeFixedCommit(c);
        }

        void CommitFixedUpdate(const Runtime::FixedStepContext &c) noexcept override {
            host_.CommitRuntimeFixedUpdate(c);
        }

        void AbortFixedUpdate(const Runtime::FixedStepContext &c) noexcept override {
            host_.AbortRuntimeFixedUpdate(c);
        }

        bool forgedRejected{};
        Runtime::RuntimeDispatchEvidence evidence;

    private:
        GuiScreenHost &host_;
    };

    class GuiFixedGate final : public Runtime::RuntimeLifecycleParticipant {
    public:
        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {}

        Result<void> OnPhase(Runtime::RuntimePhase, const Runtime::FrameContext &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &) override {
            return Result<void>::Success();
        }

        Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &) override {
            if (retire)
                scheduler->RetireDispatch();
            if (fail)
                throw std::runtime_error("Injected later fixed owner failure.");
            return Result<void>::Success();
        }

        Runtime::FrameScheduler *scheduler{};
        bool fail{}, retire{};
    };

    void CheckDeferredScreenChanges(GuiScreenHost &host, ScreenStats &stats, const int scenario) {
        stats.fixedAction = FixedAction::None;
        host.OnUpdate(0);
        if (scenario == 1 || scenario == 2 || scenario == 4)
            CHECK(stats.destructions == 1);
        if (scenario == 3) {
            // Exception unwinding must restore callback state, otherwise this navigation remains queued.
            REQUIRE(host.Navigate({GuiRouteKind::ProjectCreation, ProjectCreationRouteParameters{}}).HasValue());
            CHECK(stats.destructions == 1);
        }
        if (scenario == 7) {
            CHECK_FALSE(host.IsApplicationCloseRequested());
            CHECK(stats.destructions == 0);
        }
    }

    void CheckFixedScreenTransaction(GuiScreenHost &host, ScreenStats &stats, Runtime::FrameScheduler &scheduler,
                                     DeterministicClock &clock) {
        const auto scenario = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
        stats.fixedHost = &host;
        stats.source = scheduler.DispatchSource();
        REQUIRE(host.BindRuntimeDispatch(stats.source).HasError());
        REQUIRE(host.BindRuntimeDispatch({}).HasError());
        stats.fixedAction = scenario < 5 ? static_cast<FixedAction>(scenario) : FixedAction::None;
        if (scenario == 7) {
            stats.fixedAction = FixedAction::Close;
            stats.denyLeave = true;
        }
        Runtime::RuntimeLifecycle lifecycle;
        auto forwarding = std::make_unique<GuiFixedParticipant>(host);
        auto *forwardingView = forwarding.get();
        REQUIRE(lifecycle.AddParticipant(std::move(forwarding)).HasValue());
        auto gate = std::make_unique<GuiFixedGate>();
        gate->scheduler = &scheduler;
        gate->fail = scenario == 5;
        gate->retire = scenario == 6;
        REQUIRE(lifecycle.AddParticipant(std::move(gate)).HasValue());
        CancellationSource cancellation;
        const auto token = cancellation.Token();
        REQUIRE(lifecycle.Startup(token).HasValue());
        REQUIRE(scheduler.RunFrame(lifecycle, token, false).HasValue());
        clock.Advance(Duration::FromMilliseconds(10));
        const auto result = scheduler.RunFrame(lifecycle, token, false);
        const bool aborted = scenario == 2 || scenario == 3 || scenario == 5 || scenario == 6;
        CHECK(result.HasError() == aborted);
        CHECK(stats.fixedUpdates == 1);
        CHECK(stats.fixedCommits == (aborted ? 0 : 1));
        CHECK(stats.fixedAborts == (aborted ? 1 : 0));
        CHECK(stats.destructions == 0);
        CHECK(stats.leaves == 0);
        CHECK(forwardingView->forgedRejected);
        if (!aborted)
            CHECK(stats.committedProof);
        Runtime::FixedStepContext fabricated{.simulationTick = 1, .fixedDelta = Duration::FromMilliseconds(10), .cancellation = token};
        CHECK(host.OnRuntimeFixedUpdate(fabricated).HasError());
        fabricated.dispatchEvidence = forwardingView->evidence;
        CHECK(host.OnRuntimeFixedUpdate(fabricated).HasError());
        CHECK(stats.fixedUpdates == 1);
        CheckDeferredScreenChanges(host, stats, scenario);
        lifecycle.Shutdown();
    }

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
    void CheckUpdatesAfterSettingsClose(GuiScreenHost &host, EditorModalHost &modals, EditorGuiContext &gui, JobSystem &jobs) {
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
        };

        UpdateBinding binding{gui};

        gui.updates = &session;
        session.SetAutomaticDownload(true);
        session.SetInstallOnExit(true);
        host.DispatchMenuInvocation({.action = EditorMenuAction::OpenEditorSettings});
        modals.OnUpdate(0.0F);
        REQUIRE(modals.TopModal() != nullptr);
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

    void ShutdownAndCheckGuiScreenHost(GuiScreenHost &host, const ScreenStats &stats, const JobSystem &jobs) {
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

    /** @brief Checks that composition keeps application services borrowed and publishes the host-owned screen registry. */
    void CheckApplicationServiceBorrows(const GuiScreenHost &host, const JobSystem &jobs, const Input::InputRouter &input,
                                        const EditorModalHost &modals, const EditorSettingsService &settings) {
        REQUIRE((&host.Services().Get<JobSystem>() == &jobs));
        REQUIRE((&host.Services().Get<Input::InputRouter>() == &input));
        REQUIRE((&host.Services().Get<EditorModalHost>() == &modals));
        REQUIRE((&host.Services().Get<EditorSettingsService>() == &settings));
        REQUIRE((&host.Services().Get<ScreenRegistry>() == &host.Screens()));
    }

    void CheckInventoryActivityShutdown(GuiScreenHost &host, const Extensions::ExtensionInventory &inventory, const bool admitted) {
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
    }

    Extensions::ExtensionInventory InstallActivityInventory(const std::filesystem::path &root, const std::filesystem::path &package) {
        Extensions::ExtensionInventory inventory{root};
        REQUIRE(inventory.InstallFromDirectory(package).HasValue());
        REQUIRE(inventory.SetEnabled("fixture.package", true).HasValue());
        REQUIRE(inventory.SetTrusted("fixture.package", true).HasValue());
        return inventory;
    }

    static_assert(!std::is_copy_constructible_v<RecordingScreen> && !std::is_move_constructible_v<RecordingScreen>);

    /** @brief Exercises the real owner update without granting or constructing page authority. */
    void CheckInspectorOnlyThemeUpdate(GuiScreenHost &host) {
        REQUIRE(host.Services().TryGet<Extensions::EditorInspectorHost>() != nullptr);
        CHECK(host.Services().TryGet<Extensions::EditorPageHost>() == nullptr);
        const auto *frame = host.Services().TryGetConst<Extensions::EditorThemeFrame>();
        REQUIRE(frame != nullptr);
        REQUIRE(frame->revision != 0);
        const auto openingRevision = frame->revision;
        auto &overlay = ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg];
        overlay.x = overlay.x < 0.5F ? 0.75F : 0.25F;
        host.OnUpdate(0.0F);
        CHECK(host.Services().TryGetConst<Extensions::EditorThemeFrame>() == frame);
        CHECK(frame->revision > openingRevision);
        CHECK(frame->colors.overlay.red == overlay.x);
    }

    TEST_CASE("Inspector-only host refreshes its borrowed theme without a page host", "[unit][editor][inspector][theme]") {
        ImGuiContext *previous = ImGui::GetCurrentContext();
        const auto destroy = [previous](ImGuiContext *context) {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        };
        const std::unique_ptr<ImGuiContext, decltype(destroy)> imgui{ImGui::CreateContext(), destroy};
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        auto *font = io.Fonts->AddFontDefault();
        REQUIRE(io.Fonts->Build());
        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        Input::InputRouter input;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
        ProjectCreationService creation{jobs, engineEvents};
        LocalizationService localization{LocaleTag{"en-US"}};
        ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings());
        EditorSettingsService settings{DefaultEditorSettings(), configuration, editorEvents, localization};
        EditorModalHost modals{editorEvents, input};
        const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
        ThemeContext theme{fonts};
        EditorSettingsSnapshot settingsSnapshot = settings.Snapshot();
        EditorGuiContext gui{engineEvents, editorEvents, localization, theme, settingsSnapshot};
        RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}},
                                               "opengl"};
        GuiScreenHost host{gui, GuiScreenHostComposition{.modalHost = modals,
                                                         .settingsService = settings,
                                                         .localization = localization,
                                                         .engineEvents = engineEvents,
                                                         .creationService = creation,
                                                         .jobs = jobs,
                                                         .inputRouter = input,
                                                         .rendererAvailability = renderers,
                                                         .extensionInspectorPolicy = Extensions::Tests::InspectorSupport::Policy()}};
        CheckInspectorOnlyThemeUpdate(host);
        host.Shutdown();
        jobs.Shutdown(ShutdownPolicy::Cancel);
    }

    TEST_CASE("GUI inventory activation requires artifact authority and custom grants before publishing surfaces",
              "[unit][editor][Activity][ABI]") {
        Horo::Tests::EditorActivityPackage package;
        const Horo::Tests::OwnedTestDirectory inventoryDirectory{"horo107 GUI inventory"};
        const auto &installRoot = inventoryDirectory.Path();

        auto inventory = InstallActivityInventory(installRoot, package.root);
        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        Input::InputRouter input;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
        ProjectCreationService creation{jobs, engineEvents};
        LocalizationService localization{LocaleTag{"en-US"}};
        ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings());
        EditorSettingsService settings{DefaultEditorSettings(), configuration, editorEvents, localization};
        EditorModalHost modals{editorEvents, input};
        const Theme::Fonts fonts{};
        ThemeContext theme{fonts};
        EditorSettingsSnapshot settingsSnapshot = settings.Snapshot();
        EditorGuiContext gui{engineEvents, editorEvents, localization, theme, settingsSnapshot};
        RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}},
                                               "opengl"};
        std::shared_ptr<const Security::NativeArtifactGate> gate;
        const bool artifactTrusted = GENERATE(false, true);
        const bool compositionGranted = GENERATE(false, true);
        if (artifactTrusted)
            gate = Horo::Tests::CreateAcceptingArtifactGate();
        GuiScreenHost host{gui, GuiScreenHostComposition{.modalHost = modals,
                                                         .settingsService = settings,
                                                         .localization = localization,
                                                         .engineEvents = engineEvents,
                                                         .creationService = creation,
                                                         .jobs = jobs,
                                                         .inputRouter = input,
                                                         .rendererAvailability = renderers,
                                                         .screenRegistry = {},
                                                         .workspacePanelRegistry = {},
                                                         .extensionInventory = &inventory,
                                                         .extensionArtifactGate = std::move(gate),
                                                         .extensionUiPolicy = compositionGranted ? Horo::Tests::EditorCompositionPolicy()
                                                                                                 : Extensions::ExtensionAdmissionPolicy{}}};
        CheckInventoryActivityShutdown(host, inventory, artifactTrusted && compositionGranted);
        jobs.Shutdown(ShutdownPolicy::Cancel);
    }

    void CheckCallbackClose(GuiScreenHost &host, ScreenStats &stats) {
        stats.closeFromUpdate = &host;
        host.OnUpdate(0.0F);
        CHECK(stats.closeCallbackStayedAlive);
        CHECK(host.IsApplicationCloseRequested());
        CHECK(stats.leaves == 1);
        CHECK(stats.destructions == 1);
    }

    void CheckNativeCallbackClose(GuiScreenHost &host, ScreenStats &stats) {
        Horo::Tests::NativeGameplayFailureControl native{2};
        auto queue = host.GameplayRetirements();
        auto reserved = queue->Reserve();
        REQUIRE(reserved.HasValue());
        Gameplay::GameModuleHost loader;
        auto loaded = loader.Load(HORO_TEST_GAME_MODULE_PATH, Horo::Tests::NativeRetirementExpectation());
        REQUIRE(loaded.HasValue());
        auto candidate = std::move(loaded).Value();
        REQUIRE(candidate.diagnostic);
        REQUIRE(native.callback);
        auto admission = std::move(reserved).Value();
        admission.Retain(std::move(candidate.owner));
        CHECK(host.RequestCloseApplication().HasError());
        CHECK_FALSE(host.IsApplicationCloseRequested());
        CHECK(stats.leaves == 1);
        CHECK(stats.destructions == 1);
        CHECK(native.control.destroyed == 0);
        std::async(std::launch::async, [callback = std::move(native.callback)]() mutable {
            callback.reset();
        }).get();
        CHECK(native.control.destroyed == 0);
        host.OnFixedUpdate(1, 1.0 / 60.0);
        CHECK(host.IsApplicationCloseRequested());
        CHECK(queue->Close().HasValue());
        CHECK(native.control.destroyed == 1);
    }

    void CheckRetirementCloseAndShutdown(GuiScreenHost &host, ScreenStats &stats, JobSystem &jobs) {
        SECTION("Application close requested inside a screen callback defers destruction") {
            CheckCallbackClose(host, stats);
        }
        SECTION("Application close retains a failed native callback until its worker completes") {
            CheckNativeCallbackClose(host, stats);
        }
        ShutdownAndCheckGuiScreenHost(host, stats, jobs);
    }

    TEST_CASE("Gui Screen Host Registers Core Status And Shuts Down Safely", "[unit][editor]") {
        DeterministicClock clock;
        auto madeScheduler = Runtime::FrameScheduler::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
        REQUIRE(madeScheduler.HasValue());
        auto scheduler = std::move(madeScheduler).Value();
        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        Input::InputRouter input;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
        ProjectCreationService creation{jobs, engineEvents};
        LocalizationService localization{LocaleTag{"en-US"}};
        ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings());
        EditorSettingsService settings{DefaultEditorSettings(), configuration, editorEvents, localization};
        EditorModalHost modals{editorEvents, input};
        const Theme::Fonts fonts{};
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
        screens.Register(GuiRouteKind::ProjectCreation, [](const EditorServiceRegistry &services, const GuiRoute &) {
            return std::make_unique<RecordingScreen>(services.Get<ScreenStats>());
        });
        WorkspacePanelRegistry panels;

        GuiScreenHost host{gui, GuiScreenHostComposition{.modalHost = modals,
                                                         .settingsService = settings,
                                                         .localization = localization,
                                                         .engineEvents = engineEvents,
                                                         .creationService = creation,
                                                         .jobs = jobs,
                                                         .inputRouter = input,
                                                         .rendererAvailability = renderers,
                                                         .screenRegistry = std::move(screens),
                                                         .workspacePanelRegistry = std::move(panels)}};
        REQUIRE((host.StatusItems().Find("horo.status.backend") != nullptr));
        const auto *backgroundBuild = host.StatusItems().Find("horo.status.build");
        REQUIRE(backgroundBuild != nullptr);
        CHECK_FALSE(backgroundBuild->content.available);
        CHECK(backgroundBuild->descriptor.actionId == "horo.build.show");
        REQUIRE((host.StatusItems().Find("horo.status.cpu") == nullptr));
        CheckApplicationServiceBorrows(host, jobs, input, modals, settings);
        REQUIRE((stats.enters == 0));
        REQUIRE((host.Navigate(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}).HasError()));
        host.Services().Register(stats);
        REQUIRE((host.Start(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}).HasValue()));
        REQUIRE((stats.enters == 1));
        REQUIRE((!host.Services().Empty()));
        REQUIRE(host.BindRuntimeDispatch(scheduler->DispatchSource()).HasValue());

        const Result<void> invalidRoute = host.Navigate(GuiRoute{GuiRouteKind::Welcome, ProjectCreationRouteParameters{}});
        REQUIRE((invalidRoute.HasError()));
        REQUIRE((invalidRoute.ErrorValue().domain.Value() == "horo.editor.screens"));
        REQUIRE((invalidRoute.ErrorValue().code.Value() == "navigation.invalid_route_parameters"));

        SECTION("Actual fixed dispatch keeps screen lifetime until commit or rollback") {
            CheckFixedScreenTransaction(host, stats, *scheduler, clock);
            return;
        }

        SECTION("Closed settings does not stop discovery, automatic download, or install-on-exit") {
            CheckUpdatesAfterSettingsClose(host, modals, gui, jobs);
        }
        SECTION("Settings opening retains the active screen's original navigation admission failure") {
            const Error failure = MakeError(Navigation::NavigationSettingsError(Navigation::NavigationSettingsFailure::Closed));
            stats.navigationFailure = failure;
            host.DispatchMenuInvocation({.action = EditorMenuAction::OpenEditorSettings});
            modals.OnUpdate(0.0F);
            auto *const modal = dynamic_cast<SettingsModal *>(modals.TopModal());
            REQUIRE(modal != nullptr);
            if (modal != nullptr) {
                CHECK(modal->Draft().navigationStatus == failure.message);
                CHECK(modal->Draft().navigationSettings.Resolve() == nullptr);
            }
            REQUIRE(modals.RequestClose(ModalId{SettingsModal::kModalId}, ModalCloseReason::Cancelled).HasValue());
            modals.OnUpdate(0.0F);
        }
        CheckRetirementCloseAndShutdown(host, stats, jobs);
    }

}  // namespace

TEST_CASE("Application Build menu dispatch opens the matching modal and respects the modal gate", "[editor][gui][build_release]") {
    using namespace Horo;
    using namespace Horo::Editor;
    EngineDataBus engineEvents;
    EditorDataBus editorEvents;
    Input::InputRouter input;
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
    ProjectCreationService creation{jobs, engineEvents};
    LocalizationService localization{LocaleTag{"en-US"}};
    ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings());
    EditorSettingsService settings{DefaultEditorSettings(), configuration, editorEvents, localization};
    EditorModalHost modals{editorEvents, input};
    const Theme::Fonts fonts{};
    ThemeContext theme{fonts};
    EditorSettingsSnapshot snapshot = settings.Snapshot();
    EditorGuiContext gui{engineEvents, editorEvents, localization, theme, snapshot};
    RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}},
                                           "opengl"};
    GuiScreenHost host{gui, GuiScreenHostComposition{.modalHost = modals,
                                                     .settingsService = settings,
                                                     .localization = localization,
                                                     .engineEvents = engineEvents,
                                                     .creationService = creation,
                                                     .jobs = jobs,
                                                     .inputRouter = input,
                                                     .rendererAvailability = renderers,
                                                     .extensionInspectorPolicy = Extensions::Tests::InspectorSupport::Policy()}};
    for (const auto action : {EditorMenuAction::OpenBuild, EditorMenuAction::OpenRelease}) {
        host.DispatchMenuInvocation({action});
        REQUIRE(modals.HasOpenModal());
        auto *modal = modals.TopModal();
        REQUIRE(modal != nullptr);
        if (action == EditorMenuAction::OpenBuild)
            CHECK(dynamic_cast<BuildModal *>(modal) != nullptr);
        else
            CHECK(dynamic_cast<BuildReleaseModal *>(modal) != nullptr);
        host.DispatchMenuInvocation({EditorMenuAction::OpenBuild});
        CHECK(modals.TopModal() == modal);
        REQUIRE(modals.RequestClose(modal->Id(), ModalCloseReason::Cancelled).HasValue());
        modals.OnUpdate(0);
        CHECK_FALSE(modals.HasOpenModal());
    }
    host.Shutdown();
    jobs.Shutdown(ShutdownPolicy::Cancel);
}
