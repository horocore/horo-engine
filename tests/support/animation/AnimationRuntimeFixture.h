#pragma once
#include "CommittedSceneAssets.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Scene/SceneAnimationRuntime.h"
#include "animation/AnimationCookFixture.h"

#include <chrono>
#include <functional>
#include <thread>

namespace Horo::Animation::Test::Scene {
    using Runtime::EntityRef;

    struct Forwarder final : Runtime::RuntimeLifecycleParticipant {
        std::unique_ptr<Runtime::SceneAnimationRuntime> adapter;
        bool enabled{true};
        bool evaluated{};
        std::uint64_t lastTick{};
        std::size_t publications{}, aborts{};

        explicit Forwarder(std::unique_ptr<Runtime::SceneAnimationRuntime> owner) : adapter(std::move(owner)) {}

        Result<void> Startup(const CancellationToken &token) override {
            return adapter->Startup(token);
        }

        Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
            return adapter->OnPhase(phase, context);
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
            if (!enabled)
                return Result<void>::Success();
            evaluated = true;
            lastTick = context.simulationTick;
            return adapter->OnFixedUpdate(context);
        }

        Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &context) override {
            return evaluated ? adapter->PrepareFixedCommit(context) : Result<void>::Success();
        }

        void CommitFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            if (evaluated) {
                adapter->CommitFixedUpdate(context);
                ++publications;
            }
            evaluated = false;
        }

        void AbortFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            if (evaluated) {
                adapter->AbortFixedUpdate(context);
                ++aborts;
            }
            evaluated = false;
        }

        void Shutdown() noexcept override {
            adapter->Shutdown();
        }
    };

    struct Peer final : Runtime::RuntimeLifecycleParticipant {
        bool failUpdate{};
        Runtime::FrameScheduler *retireBeforeFence{};
        Runtime::SceneAnimationRuntime *closeDuringUpdate{};
        Runtime::SceneAnimationRuntime *closeDuringPrepare{};
        CancellationSource *cancelDuringPrepare{};

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnPhase(Runtime::RuntimePhase, const Runtime::FrameContext &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &) override {
            if (closeDuringUpdate)
                closeDuringUpdate->Shutdown();
            if (std::exchange(failUpdate, false))
                return Result<void>::Failure({ErrorCode{"test.animation.peer_failed"},
                                              ErrorDomainId{"test.animation"},
                                              ErrorSeverity::Error,
                                              "Injected later owner failure.",
                                              {}});
            return Result<void>::Success();
        }

        Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &) override {
            if (retireBeforeFence)
                retireBeforeFence->RetireDispatch();
            if (closeDuringPrepare)
                closeDuringPrepare->Shutdown();
            if (cancelDuringPrepare)
                cancelDuringPrepare->RequestCancellation();
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {}
    };

    using SourcePreparation = std::function<std::optional<AnimationAuthoringComponent>(Cook::Fixture &)>;

    inline std::unique_ptr<Application::Internal::CommittedSceneAssets> CaptureRuntimeAssets(
        Cook::Fixture &cook, const SourcePreparation &prepareSources, std::optional<AnimationAuthoringComponent> &binding) {
        if (prepareSources)
            binding = prepareSources(cook);
        const auto report = Cook::Take(cook.Cook());
        const auto contents = cook.Contents(report);
        REQUIRE(cook.prepared->ValidateEmitted(Cook::Fixture::Views(contents), {}).HasValue());
        return Cook::Take(Application::Internal::CommittedSceneAssets::Capture(*cook.request.publicationFiles,
                                                                               {cook.cooked.path, cook.request.target},
                                                                               cook.registry.Snapshot()));
    }

    struct Fixture final {
        Cook::Fixture cook;
        std::optional<AnimationAuthoringComponent> sourceBinding;
        std::unique_ptr<Application::Internal::CommittedSceneAssets> committed;
        JobSystem loadJobs{{1, 8}};
        Assets::AssetLoadService loads{loadJobs, committed->SceneProvider()};
        DeterministicClock clock;
        CancellationSource lifetime;
        CancellationToken token{lifetime.Token()};
        std::unique_ptr<Runtime::FrameScheduler> scheduler;
        Runtime::RuntimeSceneService scenes{cook.registry, loads};
        Runtime::RuntimeLifecycle lifecycle;
        Forwarder *forward{};
        Peer *peer{};
        EntityRef entity;
        bool clipSource{};

        explicit Fixture(bool directClip = false, bool withStructuralOwner = true, const SourcePreparation &prepareSources = {})
            : committed(CaptureRuntimeAssets(cook, prepareSources, sourceBinding)), clipSource(directClip) {
            scheduler = Cook::Take(Runtime::FrameScheduler::Create(clock, {.fixedStep = Duration::FromNanoseconds(10)}));
            Runtime::SceneAnimationRuntimeConfig config;
            config.expectedDispatch = scheduler->DispatchSource();
            config.runtimeOwner = Id<AnimationRuntimeId>(501);  // Explicit test-owned namespace, never derived from a Scene ID.
            config.target = cook.request.target;
            config.ownerCancellation = token;
            auto adapter = Cook::Take(Runtime::SceneAnimationRuntime::Create(config));
            REQUIRE(scenes.AddActivationParticipant(adapter->MakeActivationParticipant()).HasValue());
            if (withStructuralOwner)
                REQUIRE(scenes.AddStructuralParticipant(adapter->MakeStructuralParticipant()).HasValue());
            auto forwarding = std::make_unique<Forwarder>(std::move(adapter));
            forward = forwarding.get();
            REQUIRE(lifecycle.AddParticipant(std::move(forwarding)).HasValue());
            auto later = std::make_unique<Peer>();
            peer = later.get();
            REQUIRE(lifecycle.AddParticipant(std::move(later)).HasValue());
            REQUIRE(lifecycle.Startup(token).HasValue());
            REQUIRE(scenes.Startup(token).HasValue());
            Activate(1);
            REQUIRE(scheduler->RunFrame(lifecycle, token, false).HasValue());
        }

        ~Fixture() {
            lifecycle.Shutdown();
            scenes.Shutdown();
            loads.Shutdown();
            loadJobs.Shutdown(ShutdownPolicy::Drain);
        }

        AnimationAuthoringComponent Binding() const {
            if (sourceBinding)
                return *sourceBinding;
            AnimationAuthoringComponent component;
            component.component = Id<AnimationComponentId>(7);
            component.skeleton = cook.native.skeleton->Data().skeleton;
            component.source.kind = clipSource ? AnimationSourceKind::Clip : AnimationSourceKind::Graph;
            if (clipSource)
                component.source.clip = cook.native.compileClips.front().Data().descriptor.id;
            else
                component.source.graph = cook.native.program->Id();
            return component;
        }

        Runtime::RuntimeSceneDefinition Definition(std::uint64_t revision) const {
            Runtime::SceneDefinitionBuilder builder{{31}, {revision}};
            Runtime::RuntimeEntityDefinition definition;
            definition.object = {1};
            definition.components.animation = Binding();
            builder.Add(std::move(definition));
            return Cook::Take(std::move(builder).Build());
        }

        std::optional<Error> Pump(std::uint64_t revision) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                auto phase = scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                            Runtime::FrameContext{1, {}, 0, 0, {}, false, token});
                if (phase.HasError())
                    return std::move(phase).ErrorValue();
                if (auto failure = scenes.TakeOperationError())
                    return failure;
                const auto view = scenes.ActiveScene();
                if (view && view->DefinitionRevision() == Runtime::SceneDefinitionRevision{revision})
                    return {};
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            FAIL("Actual Animation Scene preparation did not terminate");
            return {};
        }

        void Activate(std::uint64_t revision) {
            REQUIRE(scenes.QueuePreparation(Definition(revision)).HasValue());
            const auto failure = Pump(revision);
            INFO((failure ? failure->code.Value() + ": " + failure->message : "success"));
            REQUIRE_FALSE(failure);
            entity = *scenes.ActiveScene()->Find(Runtime::SceneObjectId{1});
        }

        Result<void> Tick() {
            clock.Advance(Duration::FromNanoseconds(10));
            return scheduler->RunFrame(lifecycle, token, false);
        }

        std::shared_ptr<const Runtime::SceneStructuralReceipt> Structural(Runtime::SceneCommandBuffer commands) {
            auto receipt = Cook::Take(scenes.QueueTrackedStructuralCommands(std::move(commands)));
            auto phase =
                scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Runtime::FrameContext{1, {}, 0, 0, {}, false, token});
            INFO((phase.HasError() ? phase.ErrorValue().code.Value() : "phase success"));
            REQUIRE(phase.HasValue());
            REQUIRE(receipt->Complete());
            return receipt;
        }

        Runtime::SceneAnimationObservation Observation() const {
            return Cook::Take(forward->adapter->Query(entity));
        }

        float PoseX() const {
            const auto observation = Observation();
            REQUIRE(observation.component);
            std::array<Math::Transform, 1> pose;
            REQUIRE(forward->adapter->CopyPose(entity, observation.component->currentPose, pose).HasValue());
            return pose[0].translation.x;
        }
    };
}  // namespace Horo::Animation::Test::Scene
