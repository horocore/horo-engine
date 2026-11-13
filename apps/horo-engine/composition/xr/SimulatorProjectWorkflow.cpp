#include "SimulatorProjectWorkflow.h"

#include "EditorSimulatorSceneSource.h"
#include "EditorXrSceneGeometry.h"
#include "GameplayWorldComposition.h"
#include "HeadlessProjectScene.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "SimulatorPlayHost.h"
#include "SimulatorProjectArtifacts.h"

#include <atomic>
#include <limits>
#include <optional>

namespace Horo::Application::Internal {
    namespace {
        Result<Runtime::SceneRuntimeId> ReserveSceneOccurrence() {
            // This executable/test workflow cohort reserves one occurrence per owner. No reload API exists.
            // This is not a process-global allocator for unrelated Scene services or external hosts.
            static std::atomic<std::uint64_t> next{1};
            auto value = next.load(std::memory_order_relaxed);
            while (value != std::numeric_limits<std::uint64_t>::max()) {
                if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                    return Result<Runtime::SceneRuntimeId>::Success({value});
            }
            return Result<Runtime::SceneRuntimeId>::Failure(MakeError(XR::XRErrors::CapacityExceeded));
        }

        class ScenarioClock final : public Clock {
        public:
            Duration elapsed;

            Duration MonotonicNow() const override {
                return elapsed;
            }
        };
    }  // namespace

    struct SimulatorProjectWorkflow::Impl final {
        class Driver;
        ScenarioClock clock;  // The one explicit headless host clock; it issues no scheduler evidence itself.
        SimulatorProjectOptions options;
        const Runtime::SceneRuntimeId sceneOccurrence;
        std::unique_ptr<HeadlessProjectSceneResources> resources;
        std::unique_ptr<Editor::EditorSimulatorSceneSource> source;
        Input::InputRouter router;
        Input::RawInputCollector raw;
        Input::InputContextToken context;
        Input::GameplayInputFrameBuilder inputs{Input::ActionId{"gameplay.move"}, {}, {}, {}};
        std::shared_ptr<SimulatorPlayHost> simulator;
        std::shared_ptr<SimulatorProjectArtifacts> artifacts;
        std::unique_ptr<Editor::XRSimulatorModel> model;
        std::optional<Runtime::RuntimeSimulationPauseLease> pause;
        std::optional<Runtime::RuntimeSingleStepReceipt> step;
        Driver *driver{};
        std::unique_ptr<Runtime::RuntimeHost> host;  // Dies first: Driver/Scene/Gameplay retire before borrowed parents.

        explicit Impl(SimulatorProjectOptions value, Runtime::SceneRuntimeId occurrence)
            : options(std::move(value)), sceneOccurrence(occurrence) {}

        ~Impl() {
            if (artifacts)
                artifacts->Revoke();
            if (host)
                host->Shutdown();
            if (simulator)
                simulator->ReleaseInputOwner();
        }

        [[nodiscard]] Result<void> PrepareResources();
        [[nodiscard]] Result<void> PrepareHost();
        [[nodiscard]] Result<void> PrepareScene();
        [[nodiscard]] Result<void> Run();
    };

    class SimulatorProjectWorkflow::Impl::Driver final : public Runtime::RuntimeLifecycleParticipant {
    public:
        Impl &owner;
        std::unique_ptr<Runtime::RuntimeSceneService> scenes;
        std::unique_ptr<GameplayWorldComposition> gameplay;
        GameplayWorldComposition *active{};

        explicit Driver(Impl &value) : owner(value), scenes(std::make_unique<Runtime::RuntimeSceneService>(value.sceneOccurrence)) {}

        Result<void> Startup(const CancellationToken &cancellation) override {
            return scenes->Startup(cancellation);
        }

        Result<void> EnsureGameplay() {
            const auto scene = scenes->ActiveScene();
            if (gameplay || !scene)
                return Result<void>::Success();
            const GameplayWorldSelection selection{.moduleId = "game.xr_simulator",
                                                   .scriptSource = owner.options.projectRoot / owner.options.scriptPath,
                                                   .scriptSidecar = owner.options.projectRoot / (owner.options.scriptPath + ".meta")};
            auto created = GameplayWorldComposition::Create(*scenes, nullptr, scene->RuntimeId().value, selection);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            gameplay = std::move(created).Value();
            active = gameplay.get();
            return owner.simulator->SetPlayState(Editor::EditorPlayState::Playing);
        }

        Result<void> OnPhase(const Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
            if (phase == Runtime::RuntimePhase::BeginFrame) {
                const auto nanoseconds = context.presentationAdmittedDuration.ToNanoseconds();
                const auto sampleTime =
                    nanoseconds >= 0 ? std::optional{Input::InputSampleTime{static_cast<std::uint64_t>(nanoseconds) / 1000}} : std::nullopt;
                owner.raw.BeginFrame(context.frameNumber, sampleTime);
                if (auto ready = EnsureGameplay(); ready.HasError())
                    return ready;
            }
            if (phase == Runtime::RuntimePhase::BuildInputSnapshot) {
                if (auto sampled = owner.simulator->BuildInputSnapshot(context, owner.raw.Commit()); sampled.HasError())
                    return sampled;
                owner.inputs.Capture(owner.router, owner.context);
            }
            if (auto advanced = gameplay ? gameplay->OnPhase(phase, context) : scenes->OnPhase(phase, context); advanced.HasError())
                return advanced;
            return UpdateView(phase);
        }

        Result<void> UpdateView(const Runtime::RuntimePhase phase) {
            if (phase != Runtime::RuntimePhase::EndFrame)
                return Result<void>::Success();
            const auto scene = scenes->ActiveScene();
            return scene ? owner.simulator->UpdateScene(*scene) : Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
            if (!gameplay)
                return Result<void>::Success();
            const auto input = owner.inputs.Consume(context.simulationTick);
            const Gameplay::GameplayInputAction action{Gameplay::GameplayActionId{"gameplay.move"},
                                                       input.moveX,
                                                       input.moveY,
                                                       input.moveDown,
                                                       input.movePressed,
                                                       input.moveReleased};
            return gameplay->FixedTick(context, {&action, 1});
        }

        void Shutdown() noexcept override {
            active = nullptr;
            gameplay.reset();
            scenes->Shutdown();
            owner.simulator->RevokeInput();
        }
    };

    Result<void> SimulatorProjectWorkflow::Impl::PrepareResources() {
        auto captured =
            PrepareHeadlessProjectSceneResources({.projectRoot = options.projectRoot,
                                                  .scene = "source:" + options.scenePath,
                                                  .publicationNamespace = std::string{SimulatorProjectArtifacts::PublicationDirectory}});
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        resources = std::move(captured).Value();
        if (!resources->authoredSource)
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        auto authored = Editor::EditorSimulatorSceneSource::Create(std::move(*resources->authoredSource), resources->definition->Id());
        if (authored.HasError())
            return Result<void>::Failure(authored.ErrorValue());
        source = std::move(authored).Value();
        auto io = SimulatorProjectArtifacts::Create({.source = resources->sourceFiles,
                                                     .publication = resources->publication,
                                                     .context = resources->sourceContext,
                                                     .authorizedRoot = options.projectRoot});
        if (io.HasError())
            return Result<void>::Failure(io.ErrorValue());
        artifacts = std::move(io).Value();
        return Result<void>::Success();
    }

    Result<void> SimulatorProjectWorkflow::Impl::PrepareHost() {
        context = router.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
        if (auto map = router.SetActionMap({Input::ActionDescriptor{Input::ActionId{"gameplay.move"}, Input::ActionValueType::Axis2D,
                                                                    Input::InputContextId{"editor.workspace"}}});
            map.HasError())
            return map;
        auto runtime = Runtime::RuntimeHost::Create(clock);
        if (runtime.HasError())
            return Result<void>::Failure(runtime.ErrorValue());
        host = std::move(runtime).Value();
        auto play =
            SimulatorPlayHost::Create({router, host->DispatchSource(), Runtime::FrameSchedulerConfig{}.fixedStep, options.trackingRoot});
        if (play.HasError())
            return Result<void>::Failure(play.ErrorValue());
        simulator = std::move(play).Value();
        model = std::make_unique<Editor::XRSimulatorModel>(simulator);
        auto collection = router.ActionCollection(context);
        if (collection.HasError())
            return Result<void>::Failure(collection.ErrorValue());
        return simulator->AttachInput(collection.Value(), source->Port());
    }

    Result<void> SimulatorProjectWorkflow::Impl::PrepareScene() {
        auto participant = std::make_unique<Driver>(*this);
        driver = participant.get();
        if (auto structural = driver->scenes->AddStructuralParticipant(MakeGameplayStructuralParticipant(driver->active));
            structural.HasError())
            return structural;
        if (auto scene = driver->scenes->AddActivationParticipant(simulator->MakeSceneParticipant()); scene.HasError())
            return scene;
        if (auto queued = driver->scenes->QueuePreparation(source->Definition()); queued.HasError())
            return queued;
        if (auto added = host->AddParticipant(std::move(participant)); added.HasError())
            return added;
        return host->Startup();
    }

    Result<void> SimulatorProjectWorkflow::Impl::Run() {
        constexpr auto quantum = 16666667;
        if (clock.elapsed.ToNanoseconds() > std::numeric_limits<std::int64_t>::max() - quantum)
            return Result<void>::Failure(MakeError(XR::XRErrors::CapacityExceeded));
        clock.elapsed += Duration::FromNanoseconds(quantum);
        if (auto frame = host->RunFrame(); frame.HasError())
            return frame;
        if (auto error = driver->scenes->TakeOperationError())
            return Result<void>::Failure(*error);
        return Result<void>::Success();
    }

    SimulatorProjectWorkflow::SimulatorProjectWorkflow(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    SimulatorProjectWorkflow::~SimulatorProjectWorkflow() = default;

    /** @copydoc SimulatorProjectWorkflow::Create */
    Result<std::unique_ptr<SimulatorProjectWorkflow>> SimulatorProjectWorkflow::Create(SimulatorProjectOptions options) {
        using Output = Result<std::unique_ptr<SimulatorProjectWorkflow>>;
        const auto reserved = ReserveSceneOccurrence();
        if (reserved.HasError())
            return Output::Failure(reserved.ErrorValue());
        auto impl = std::make_unique<Impl>(std::move(options), reserved.Value());
        if (auto captured = impl->PrepareResources(); captured.HasError())
            return Output::Failure(captured.ErrorValue());
        if (auto runtime = impl->PrepareHost(); runtime.HasError())
            return Output::Failure(runtime.ErrorValue());
        if (auto scene = impl->PrepareScene(); scene.HasError())
            return Output::Failure(scene.ErrorValue());
        for (std::size_t attempt = 0; attempt < 8 && !impl->driver->gameplay; ++attempt)
            if (auto warm = impl->Run(); warm.HasError())
                return Output::Failure(warm.ErrorValue());
        if (!impl->driver->gameplay)
            return Output::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        return Output::Success(std::unique_ptr<SimulatorProjectWorkflow>{new SimulatorProjectWorkflow(std::move(impl))});
    }

    /** @copydoc SimulatorProjectWorkflow::Frame */
    Result<void> SimulatorProjectWorkflow::Frame() {
        return impl_->Run();
    }

    /** @copydoc SimulatorProjectWorkflow::Pause */
    Result<void> SimulatorProjectWorkflow::Pause() {
        if (impl_->pause)
            return Result<void>::Success();
        auto &control = impl_->host->SimulationControl();
        const auto policy = control.ReadPolicy();
        if (policy.HasError())
            return Result<void>::Failure(policy.ErrorValue());
        auto lease = control.AcquirePause(Runtime::RuntimeSimulationPauseReason::Debugger, policy.Value());
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        impl_->pause.emplace(std::move(lease).Value());
        return impl_->simulator->SetPlayState(Editor::EditorPlayState::Paused);
    }

    /** @copydoc SimulatorProjectWorkflow::Resume */
    Result<void> SimulatorProjectWorkflow::Resume() {
        impl_->pause.reset();
        impl_->step.reset();
        return impl_->simulator->SetPlayState(Editor::EditorPlayState::Playing);
    }

    /** @copydoc SimulatorProjectWorkflow::Step */
    Result<void> SimulatorProjectWorkflow::Step() {
        if (!impl_->pause)
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        auto &control = impl_->host->SimulationControl();
        const auto policy = control.ReadPolicy();
        if (policy.HasError())
            return Result<void>::Failure(policy.ErrorValue());
        auto requested = control.RequestStep(policy.Value());
        if (requested.HasError())
            return Result<void>::Failure(requested.ErrorValue());
        impl_->step.emplace(std::move(requested).Value());
        const auto view = impl_->model->Snapshot();
        return impl_->model->Apply({view.state.session.session, Editor::XRSimulatorCommandKind::Step, {}});
    }

    /** @copydoc SimulatorProjectWorkflow::Model */
    Editor::XRSimulatorModel &SimulatorProjectWorkflow::Model() noexcept {
        return *impl_->model;
    }

    /** @copydoc SimulatorProjectWorkflow::Artifacts */
    std::shared_ptr<Editor::IXRSimulatorArtifacts> SimulatorProjectWorkflow::Artifacts() const noexcept {
        return impl_->artifacts;
    }

    /** @copydoc SimulatorProjectWorkflow::Position */
    Result<Math::Vec3> SimulatorProjectWorkflow::Position(const Runtime::SceneObjectId object) const {
        const auto scene = impl_->driver->scenes->ActiveScene();
        if (!scene)
            return Result<Math::Vec3>::Failure(MakeError(XR::XRErrors::IdentityStale));
        const auto entity = scene->Find(object);
        if (!entity)
            return Result<Math::Vec3>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        const auto view = scene->Get(*entity);
        if (view.HasError())
            return Result<Math::Vec3>::Failure(view.ErrorValue());
        return Result<Math::Vec3>::Success(view.Value().localTransform->translation);
    }

    /** @copydoc SimulatorProjectWorkflow::Preview */
    std::optional<Editor::EditorPlayXrPreview> SimulatorProjectWorkflow::Preview() const noexcept {
        return impl_->simulator->Preview();
    }

    /** @copydoc SimulatorProjectWorkflow::SceneIdentity */
    Runtime::SceneRuntimeId SimulatorProjectWorkflow::SceneIdentity() const noexcept {
        const auto view = impl_->driver->scenes->ActiveScene();
        return view ? view->RuntimeId() : Runtime::SceneRuntimeId{};
    }
}  // namespace Horo::Application::Internal
