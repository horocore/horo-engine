#include "../../helpers/GameplayModuleTestSupport.h"
#include "DecisionGraphCompilerTestSupport.h"
#include "EditorPlayXrHost.h"
#include "EditorProjectSceneHost.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/PerceptionSenseProvider.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "PrefabSceneCookHostFixture.h"
#include "assets/AssetCookOutputFixture.h"
#include "editor/document/EditorAssetMeshCache.h"
#include "editor/gameplay/EditorPlaySessionController.h"
#include "editor/renderer/xr/EditorXrSceneGeometry.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;

    class MovingBehavior final : public Gameplay::IBehaviorInstance {
    public:
        void OnFixedUpdate(Gameplay::BehaviorContext &context, Gameplay::FixedDeltaTime) override {
            auto transform = context.LocalTransform();
            REQUIRE(transform.HasValue());
            Math::Transform moved = transform.Value();
            moved.translation.x += 1.0F;
            REQUIRE(context.SetLocalTransform(moved).HasValue());
        }
    };

    class FastMovingBehavior final : public Gameplay::IBehaviorInstance {
    public:
        void OnFixedUpdate(Gameplay::BehaviorContext &context, Gameplay::FixedDeltaTime) override {
            auto transform = context.LocalTransform();
            REQUIRE(transform.HasValue());
            Math::Transform moved = transform.Value();
            moved.translation.x += 2.0F;
            REQUIRE(context.SetLocalTransform(moved).HasValue());
        }
    };

    Gameplay::IBehaviorInstance *CreateBehavior(void *) {
        return new MovingBehavior{};
    }

    void DestroyBehavior(void *, Gameplay::IBehaviorInstance *instance) noexcept {
        delete instance;
    }

    Gameplay::IBehaviorInstance *CreateFastBehavior(void *) {
        return new FastMovingBehavior{};
    }

    void DestroyFastBehavior(void *, Gameplay::IBehaviorInstance *instance) noexcept {
        delete instance;
    }

    Gameplay::BehaviorTypeId BehaviorType() {
        auto parsed = Gameplay::BehaviorTypeId::Parse("game.tests.play_mover");
        REQUIRE(parsed.HasValue());
        return std::move(parsed).Value();
    }

    Gameplay::BehaviorRegistry Registry() {
        Gameplay::BehaviorRegistry registry;
        Gameplay::BehaviorDescriptor descriptor;
        descriptor.typeId = BehaviorType();
        descriptor.displayName = "Play Mover";
        descriptor.phases.push_back({Gameplay::BehaviorPhase::Gameplay, "game.tests.play_mover", {}, {}, {}});
        REQUIRE(registry.Register({std::move(descriptor), {nullptr, &CreateBehavior, &DestroyBehavior}}).HasValue());
        REQUIRE(registry.Freeze().HasValue());
        return registry;
    }

    Gameplay::BehaviorRegistry FastRegistry() {
        Gameplay::BehaviorRegistry registry;
        Gameplay::BehaviorDescriptor descriptor;
        descriptor.typeId = BehaviorType();
        descriptor.displayName = "Fast Play Mover";
        descriptor.phases.push_back({Gameplay::BehaviorPhase::Gameplay, "game.tests.play_mover", {}, {}, {}});
        REQUIRE(registry.Register({std::move(descriptor), {nullptr, &CreateFastBehavior, &DestroyFastBehavior}}).HasValue());
        REQUIRE(registry.Freeze().HasValue());
        return registry;
    }

    Gameplay::ComponentTypeId ComponentType() {
        auto parsed = Gameplay::ComponentTypeId::Parse("game.tests.play_component");
        REQUIRE(parsed.HasValue());
        return std::move(parsed).Value();
    }

    Gameplay::SerializedComponent ComponentPayload() {
        return {.typeId = ComponentType(),
                .schemaVersion = 1,
                .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                .payload = {std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}}};
    }

    Gameplay::ComponentRegistry ComponentRegistry() {
        Gameplay::ComponentRegistry registry;
        REQUIRE(registry.Register({.typeId = ComponentType(), .schemaVersion = 1, .displayName = "Play Component"}).HasValue());
        REQUIRE(registry.Freeze().HasValue());
        return registry;
    }

    Editor::SceneDocumentSnapshot AuthoringScene() {
        Editor::SceneObjectComponentSet camera;
        camera.camera = Runtime::CameraComponent{};
        Editor::SceneObjectComponentSet actor;
        actor.behaviors.push_back({Gameplay::BehaviorInstanceId{1}, BehaviorType(), 1, true, {}});
        return {Editor::DocumentRevision{7},
                Editor::DocumentStateId{11},
                {{Editor::SceneObjectId{1}, std::nullopt, "Camera", {}, std::nullopt, std::move(camera)},
                 {Editor::SceneObjectId{2}, std::nullopt, "Actor", {}, std::nullopt, std::move(actor)}}};
    }

    /** @brief Real CPU Scene participant over canonical document/cache capture; never claims native XR qualification. */
    class CpuSelectedXrHost final : public Editor::IEditorPlayXrHost {
    public:
        explicit CpuSelectedXrHost(Editor::EditorXrSceneSourcePort source) : source_(std::move(source)) {}

        Editor::EditorPlayViewSource sourceKind{Editor::EditorPlayViewSource::SelectedXr};

        Editor::EditorPlayViewSource ViewSource() const noexcept override {
            return sourceKind;
        }

        bool failPreparation{};
        bool published{};
        Runtime::SceneRuntimeId current;
        std::size_t retired{};

        Result<void> AttachInput(const Input::InputActionCollection &, const Editor::EditorXrSceneSourcePort &) override {
            return Result<void>::Failure(MakeError(Gameplay::GameplayErrors::GameplayPlayBlocked));
        }

        Result<void> SetPlayState(Editor::EditorPlayState) override {
            return Result<void>::Success();
        }

        Result<void> DetachInput() override {
            return Result<void>::Success();
        }

        void RevokeInput() noexcept override {
            published = false;
        }

        Result<void> UpdateScene(const Runtime::RuntimeSceneView &) override {
            return Result<void>::Success();
        }

        class Candidate final : public Runtime::SceneActivationCandidate {
        public:
            Candidate(CpuSelectedXrHost &host, Runtime::SceneRuntimeId scene, std::unique_ptr<Editor::PreparedEditorXrGeometry> geometry)
                : host_(host), scene_(scene), geometry_(std::move(geometry)) {}

            Result<void> ValidatePublication() const override {
                return geometry_->ValidateSource();
            }

            void Publish() noexcept override {
                host_.current = scene_;
                host_.published = true;
            }

            void Shutdown() noexcept override {
                if (closed_)
                    return;
                closed_ = true;
                geometry_->Revoke();
                if (host_.current == scene_)
                    host_.published = false;
                ++host_.retired;
            }

        private:
            CpuSelectedXrHost &host_;
            Runtime::SceneRuntimeId scene_;
            std::unique_ptr<Editor::PreparedEditorXrGeometry> geometry_;
            bool closed_{};
        };

        class Participant final : public Runtime::SceneActivationParticipant {
        public:
            explicit Participant(CpuSelectedXrHost &host) : host_(host) {}

            Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                               const Runtime::RuntimeSceneView &scene) override {
                // Preserve the original value snapshot before the source provider can reenter its caller.
                const Runtime::RuntimeSceneView candidate = scene;
                if (host_.failPreparation)
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(
                        MakeError(Gameplay::GameplayErrors::GameplayPlayBlocked));
                auto geometry = host_.source_.Capture(definition.Revision(), candidate);
                if (geometry.HasError())
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(geometry.ErrorValue());
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                    std::make_unique<Candidate>(host_, candidate.RuntimeId(), std::move(geometry).Value()));
            }

        private:
            CpuSelectedXrHost &host_;
        };

        std::unique_ptr<Runtime::SceneActivationParticipant> MakeSceneParticipant(Editor::EditorMountedXrGeometryCapture mounted) override {
            if (mounted)
                return {};
            return std::make_unique<Participant>(*this);
        }

    private:
        Editor::EditorXrSceneSourcePort source_;
    };

    struct PlayOwnerObservations final {
        std::vector<Runtime::SceneRuntimeId> admitted;
        std::optional<Editor::EditorPlayDecisionProblem> decisionProblem;
        std::uint64_t inspectionTick{};
        std::uint32_t inspectionGeneration{1};
        std::size_t inspectionReads{};
        std::size_t ticks{};
        std::size_t closed{};
        bool failTick{};
        Runtime::RuntimeDispatchSource source;
        Runtime::RuntimeDispatchEvidence evidence;
        Runtime::RuntimeDispatchFacts updateFacts, prepareFacts, commitFacts;
        Runtime::RuntimeDispatchStatus abortStatus{};
        std::size_t prepares{}, publications{}, aborts{};
        Editor::EditorPlaySessionController *stopDuringUpdate{}, *stopDuringPrepare{};
        bool failPrepare{}, pending{}, stopRetainedOwner{};
    };

    /** @brief Exercises the injected owner protocol using an actual assetless Scene service; no native backend claim. */
    class ObservedPlaySceneHost final : public Editor::EditorPlaySceneHost {
    public:
        ObservedPlaySceneHost(Runtime::SceneRuntimeId identity, std::shared_ptr<PlayOwnerObservations> observations,
                              Editor::IEditorPlayXrHost *xr = nullptr)
            : observations_(std::move(observations)), scenes_(identity), xr_(xr) {
            if (xr_)
                REQUIRE(scenes_.AddActivationParticipant(xr_->MakeSceneParticipant()).HasValue());
        }

        const Editor::IEditorPlayXrHost *SelectedXrHost() const noexcept override {
            return xr_;
        }

        Runtime::RuntimeSceneService &Scenes() noexcept override {
            return scenes_;
        }

        Result<void> FixedUpdate(Gameplay::FixedDeltaTime) override {
            ++observations_->ticks;
            return observations_->failTick ? Result<void>::Failure(MakeError(Gameplay::GameplayErrors::PhysicsUnavailable))
                                           : Result<void>::Success();
        }

        /** @brief Supplies an owned report for the session projection test. */
        std::optional<Editor::EditorPlayDecisionProblem> ReadDecisionProblem() const override {
            return observations_->decisionProblem;
        }

        std::uint64_t AgentInspectionTick() const noexcept override {
            return observations_->inspectionTick;
        }

        std::optional<Editor::EditorPlayAgentInspection> ReadAgentInspection(Runtime::EntityRef owner) const override {
            ++observations_->inspectionReads;
            const AI::AgentHandle handle{AI::AiRuntimeIncarnation::Create(owner.runtime.value).Value(),
                                         {owner.entity.index, observations_->inspectionGeneration}};
            return Editor::EditorPlayAgentInspection{owner, handle, observations_->inspectionTick,
                                                     std::make_shared<const AI::AiAgentInspectionSnapshot>()};
        }

        Result<void> OnRuntimeFixedUpdate(const Runtime::FixedStepContext &context) override {
            observations_->evidence = context.dispatchEvidence;
            REQUIRE(context.dispatchEvidence.Read(observations_->source, Runtime::RuntimePhase::FixedUpdate, observations_->updateFacts) ==
                    Runtime::RuntimeDispatchStatus::Valid);
            observations_->pending = true;
            if (observations_->stopDuringUpdate) {
                observations_->stopDuringUpdate->Stop();
                observations_->stopRetainedOwner = observations_->closed == 0;
            }
            return FixedUpdate(Gameplay::FixedDeltaTime{static_cast<double>(context.fixedDelta.ToNanoseconds()) / 1'000'000'000.0});
        }

        Result<void> PrepareRuntimeFixedCommit(const Runtime::FixedStepContext &context) override {
            ++observations_->prepares;
            REQUIRE(context.dispatchEvidence.Read(observations_->source, Runtime::RuntimePhase::FixedUpdate, observations_->prepareFacts) ==
                    Runtime::RuntimeDispatchStatus::Valid);
            if (observations_->stopDuringPrepare) {
                observations_->stopDuringPrepare->Stop();
                observations_->stopRetainedOwner = observations_->closed == 0;
            }
            return observations_->failPrepare ? Result<void>::Failure(MakeError(Gameplay::GameplayErrors::PhysicsUnavailable))
                                              : Result<void>::Success();
        }

        void CommitRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            Runtime::RuntimeDispatchFacts facts;
            static_cast<void>(context.dispatchEvidence.Read(observations_->source, Runtime::RuntimePhase::FixedUpdate, facts));
            observations_->commitFacts = facts;
            ++observations_->publications;
            observations_->pending = false;
        }

        void AbortRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            Runtime::RuntimeDispatchFacts ignored;
            observations_->abortStatus = context.dispatchEvidence.Read(observations_->source, Runtime::RuntimePhase::FixedUpdate, ignored);
            ++observations_->aborts;
            observations_->pending = false;
        }

        void Shutdown() noexcept override {
            if (closed_)
                return;
            closed_ = true;
            ++observations_->closed;
            scenes_.Shutdown();
        }

    private:
        Editor::IEditorPlayXrHost *xr_{};
        std::shared_ptr<PlayOwnerObservations> observations_;
        Runtime::RuntimeSceneService scenes_;
        bool closed_{};
    };

    Editor::EditorPlaySceneFactory ObservePlayOwner(const std::shared_ptr<PlayOwnerObservations> &observations) {
        return [observations](Runtime::SceneRuntimeId identity,
                              const Runtime::RuntimeSceneDefinition &definition) -> Result<std::unique_ptr<Editor::EditorPlaySceneHost>> {
            REQUIRE(definition.Entities().size() == 2);
            observations->admitted.push_back(identity);
            return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Success(
                std::make_unique<ObservedPlaySceneHost>(identity, observations));
        };
    }

    /** @brief Owns the complete selected-Play source/factory lifetime through each independent admission/Stop scenario. */
    struct SelectedXrPlayFixture {
        Editor::SceneDocument document;
        Editor::EditorHistory history;
        Editor::SceneDocumentCommandExecutor commands{document, history};
        Runtime::PrimitiveMeshCache primitives;
        Editor::EditorAssetMeshCache assets;
        Editor::EditorXrSceneSourceOwner source{document, primitives, assets};
        CpuSelectedXrHost xr{source.Port()}, foreign{source.Port()};
        Gameplay::BehaviorRegistry registry;
        std::shared_ptr<PlayOwnerObservations> observations{std::make_shared<PlayOwnerObservations>()};
        Editor::EditorPlaySceneFactory factory;
        Editor::EditorPlaySessionController play;  // Dies before every authority captured by the factory.

        SelectedXrPlayFixture() {
            REQUIRE(commands.Execute(Editor::CreateSceneObjectCommand{.name = "Box", .primitiveMesh = Runtime::PrimitiveMeshDescriptor{}})
                        .HasValue());
            REQUIRE(registry.Freeze().HasValue());
            factory = [this](Runtime::SceneRuntimeId identity, const Runtime::RuntimeSceneDefinition &) {
                return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Success(
                    std::make_unique<ObservedPlaySceneHost>(identity, observations, &xr));
            };
        }
    };
}  // namespace

TEST_CASE_METHOD(SelectedXrPlayFixture,
                 "selected XR camera-free Play requires the exact real Scene participant and preserves repeat Play ownership") {
    using namespace Editor;
    SECTION("ordinary desktop still requires its authored camera") {
        REQUIRE(play.Start(document.Snapshot(), registry, factory).HasError());
        CHECK_FALSE(xr.published);
    }
    SECTION("selected enum without actual host cannot bypass admission") {
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::SelectedXr).HasError());
        CHECK_FALSE(xr.published);
    }
    SECTION("wrong actual factory host rejects before publication") {
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::SelectedXr, &foreign).HasError());
        CHECK_FALSE(xr.published);
    }
    SECTION("actual participant failure remains a Play failure") {
        xr.failPreparation = true;
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::SelectedXr, &xr).HasError());
        CHECK_FALSE(xr.published);
        CHECK_FALSE(play.SceneView());
    }
    SECTION("same source can prepare a new higher Scene occurrence after Stop") {
        REQUIRE(document.State().value != document.Revision().value);
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::SelectedXr, &xr).HasValue());
        REQUIRE(xr.published);
        const auto first = play.SceneView()->RuntimeId();
        REQUIRE(xr.current == first);
        play.Stop();
        REQUIRE_FALSE(xr.published);
        REQUIRE(xr.retired == 1);
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::SelectedXr, &xr).HasValue());
        REQUIRE(xr.published);
        REQUIRE(play.SceneView()->RuntimeId() != first);
        REQUIRE(xr.current == play.SceneView()->RuntimeId());
        play.Stop();
        REQUIRE(xr.retired == 2);
    }
}

TEST_CASE_METHOD(SelectedXrPlayFixture, "Simulator Play shares native selection factory and participant provenance checks",
                 "[editor][xr][simulator]") {
    using namespace Editor;
    SECTION("a Simulator enum cannot relabel a native host") {
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::Simulator, &xr).HasError());
        REQUIRE_FALSE(xr.published);
    }
    SECTION("a Simulator enum requires the actual application host") {
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::Simulator).HasError());
        REQUIRE_FALSE(xr.published);
    }
    SECTION("the actual factory must capture the same Simulator host") {
        xr.sourceKind = EditorPlayViewSource::Simulator;
        foreign.sourceKind = EditorPlayViewSource::Simulator;
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::Simulator, &foreign).HasError());
        REQUIRE_FALSE(xr.published);
    }
    SECTION("genuine Simulator participant prepares the camera-free actual Scene") {
        xr.sourceKind = EditorPlayViewSource::Simulator;
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::Simulator, &xr).HasValue());
        REQUIRE(xr.published);
        REQUIRE(play.SceneView().has_value());
        REQUIRE(xr.current == play.SceneView()->RuntimeId());
        play.Stop();
        REQUIRE_FALSE(xr.published);
    }
    SECTION("Simulator participant failure cannot publish Play") {
        xr.sourceKind = EditorPlayViewSource::Simulator;
        xr.failPreparation = true;
        REQUIRE(play.Start(document.Snapshot(), registry, factory, {}, EditorPlayViewSource::Simulator, &xr).HasError());
        REQUIRE_FALSE(xr.published);
        REQUIRE_FALSE(play.SceneView().has_value());
    }
}

TEST_CASE("play session pause step stop keeps authoring snapshot isolated") {
    Gameplay::BehaviorRegistry registry = Registry();
    const Editor::SceneDocumentSnapshot authoring = AuthoringScene();
    Editor::EditorPlaySessionController play;

    REQUIRE(play.Start(authoring, registry).HasValue());
    REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
    REQUIRE(play.AuthoringRevision() == authoring.revision);
    REQUIRE(play.FixedUpdate({}, Gameplay::FixedDeltaTime{1.0 / 60.0}).HasValue());

    REQUIRE(play.Pause().HasValue());
    REQUIRE(play.FixedUpdate({}, Gameplay::FixedDeltaTime{1.0 / 60.0}).HasValue());
    auto actor = play.SceneView().value().Find(Runtime::SceneObjectId{2});
    REQUIRE(actor.has_value());
    REQUIRE(play.SceneView().value().Get(*actor).Value().localTransform->translation.x == 1.0F);

    REQUIRE(play.Step().HasValue());
    REQUIRE(play.FixedUpdate({}, Gameplay::FixedDeltaTime{1.0 / 60.0}).HasValue());
    REQUIRE(play.SceneView().value().Get(*actor).Value().localTransform->translation.x == 2.0F);
    REQUIRE(authoring.objects[1].localTransform.translation.x == 0.0F);

    play.Stop();
    REQUIRE(play.State() == Editor::EditorPlaySessionState::Idle);
    REQUIRE(!play.SceneView().has_value());
    REQUIRE(authoring.revision == Editor::DocumentRevision{7});
}

TEST_CASE("play session rejects a scene without a runtime camera") {
    Gameplay::BehaviorRegistry registry = Registry();
    Editor::SceneDocumentSnapshot authoring = AuthoringScene();
    authoring.objects.erase(authoring.objects.begin());
    Editor::EditorPlaySessionController play;
    REQUIRE(play.Start(authoring, registry).HasError());
    REQUIRE(play.State() == Editor::EditorPlaySessionState::Failed);
    REQUIRE(play.LastError().has_value());
}

TEST_CASE("play session rejects a scene whose authored cameras are disabled") {
    Gameplay::BehaviorRegistry registry = Registry();
    Editor::SceneDocumentSnapshot authoring = AuthoringScene();
    authoring.objects.front().components.camera->enabled = false;
    Editor::EditorPlaySessionController play;
    REQUIRE(play.Start(authoring, registry).HasError());
    REQUIRE(play.State() == Editor::EditorPlaySessionState::Failed);
}

TEST_CASE("play session reloads behavior factories without replacing current runtime scene state") {
    Gameplay::BehaviorRegistry original = Registry();
    Gameplay::BehaviorRegistry candidate = FastRegistry();
    Editor::EditorPlaySessionController play;
    REQUIRE(play.Start(AuthoringScene(), original).HasValue());
    const Runtime::SceneRuntimeId runtimeId = play.SceneView().value().RuntimeId();
    REQUIRE(play.FixedUpdate({}, Gameplay::FixedDeltaTime{1.0 / 60.0}).HasValue());

    REQUIRE(play.ReloadBehaviors(candidate, original).HasValue());
    REQUIRE(play.SceneView().value().RuntimeId() == runtimeId);
    REQUIRE(play.FixedUpdate({}, Gameplay::FixedDeltaTime{1.0 / 60.0}).HasValue());
    const auto actor = play.SceneView().value().Find(Runtime::SceneObjectId{2});
    REQUIRE(actor.has_value());
    REQUIRE(play.SceneView().value().Get(*actor).Value().localTransform->translation.x == 3.0F);
}

TEST_CASE("play session gates missing gameplay components with affected object diagnostics") {
    Gameplay::BehaviorRegistry behaviors = Registry();
    Gameplay::ComponentRegistry missing;
    REQUIRE(missing.Freeze().HasValue());
    Editor::SceneDocumentSnapshot authoring = AuthoringScene();
    authoring.objects[1].components.gameplayComponents.push_back(ComponentPayload());

    Editor::EditorPlaySessionController play;
    REQUIRE(play.Start(authoring, behaviors, missing).HasError());
    REQUIRE(play.State() == Editor::EditorPlaySessionState::Failed);
    REQUIRE(play.LastError().has_value());
    REQUIRE(play.LastError()->code.Value() == Gameplay::GameplayErrors::GameplayPlayBlocked.code.Value());
    REQUIRE(play.LastError()->diagnostics.size() == 1);
    REQUIRE(play.LastError()->diagnostics.front().path == "objects[2].components.gameplayComponents[0]");

    Gameplay::ComponentRegistry restored = ComponentRegistry();
    REQUIRE(play.Start(authoring, behaviors, restored).HasValue());
    REQUIRE(play.SceneView().value().Find(Runtime::SceneObjectId{2}).has_value());
}

TEST_CASE("Standalone Play diagnostics bind the real clone and detach across reload stop and failure", "[unit][editor][play][network]") {
    Application::NetworkDebuggerService debugger;
    Editor::EditorPlaySessionController play{&debugger};
    auto registry = Registry();
    REQUIRE(play.Start(AuthoringScene(), registry).HasValue());
    auto first = debugger.Query();
    REQUIRE(first.state == Application::NetworkDebuggerState::Disabled);
    REQUIRE(first.snapshot.source.scene == play.SceneView().value().RuntimeId().value);
    REQUIRE(first.snapshot.source.sceneGeneration != 0);
    REQUIRE_FALSE(debugger.Request(first.snapshot.source, first.snapshot.revision, Network::NetworkCaptureAction::Pause));
    const auto reload = play.QuiesceForReload();
    REQUIRE(reload.HasValue());
    REQUIRE(debugger.Query().state == Application::NetworkDebuggerState::Detached);
    REQUIRE(play.RestoreAfterReload(registry, reload.Value()).HasValue());
    REQUIRE(debugger.Query().state == Application::NetworkDebuggerState::Disabled);
    REQUIRE(debugger.Query().snapshot.source.session > first.snapshot.source.session);
    play.Stop();
    REQUIRE(debugger.Query().state == Application::NetworkDebuggerState::Detached);
    REQUIRE(play.Start(AuthoringScene(), registry).HasValue());
    REQUIRE(debugger.Query().snapshot.source.session > first.snapshot.source.session);
    play.DegradeAfterReload(MakeError(Gameplay::GameplayErrors::GameplayReloadRestoreFailed));
    REQUIRE(debugger.Query().state == Application::NetworkDebuggerState::Detached);
}

TEST_CASE("Play injects one Scene owner and steps it only at admitted fixed quanta", "[editor][play][ownership]") {
    auto observations = std::make_shared<PlayOwnerObservations>();
    const auto factory = ObservePlayOwner(observations);
    const auto authoring = AuthoringScene();
    auto registry = Registry();
    Editor::EditorPlaySessionController play;
    REQUIRE(play.Start(authoring, registry, factory).HasValue());
    REQUIRE(play.SceneView().has_value());
    CHECK(play.SceneView()->RuntimeId() == Editor::EditorFirstPlayScene);
    REQUIRE(play.FixedUpdate({}, {1.0 / 60.0}).HasValue());
    REQUIRE(play.Pause().HasValue());
    REQUIRE(play.FixedUpdate({}, {1.0 / 60.0}).HasValue());
    CHECK(observations->ticks == 1);
    REQUIRE(play.Step().HasValue());
    REQUIRE(play.FixedUpdate({}, {1.0 / 60.0}).HasValue());
    CHECK(observations->ticks == 2);
    play.Stop();
    play.Stop();
    CHECK(observations->closed == 1);
    CHECK_FALSE(play.SceneView());
    CHECK(authoring.objects[1].localTransform.translation.x == 0);
    REQUIRE(play.Start(authoring, registry, factory).HasValue());
    CHECK(play.SceneView()->RuntimeId().value > observations->admitted.front().value);
    CHECK(observations->admitted.size() == 2);
    play.Stop();
    CHECK(observations->closed == 2);
}

TEST_CASE("Play exposes the owned Decision problem while playing and paused", "[editor][play][ai]") {
    const auto observations = std::make_shared<PlayOwnerObservations>();
    auto registry = Registry();
    Editor::EditorPlaySessionController play;
    CHECK_FALSE(play.ReadDecisionProblem());

    REQUIRE(play.Start(AuthoringScene(), registry, ObservePlayOwner(observations)).HasValue());
    REQUIRE(play.SceneView());
    CHECK_FALSE(play.ReadDecisionProblem());
    const auto error = MakeError(Gameplay::GameplayErrors::PhysicsUnavailable);
    const auto agent = AI::AgentId::Create(7).Value();
    observations->decisionProblem = Editor::EditorPlayDecisionProblem{.scene = play.SceneView()->RuntimeId(),
                                                                      .tick = 2,
                                                                      .agent = agent,
                                                                      .phase = Editor::EditorPlayDecisionPhase::Evaluate,
                                                                      .error = error,
                                                                      .entity = std::nullopt};

    const auto captured = play.ReadDecisionProblem();
    REQUIRE(captured);
    CHECK(captured->tick == 2);
    CHECK(captured->agent == agent);
    REQUIRE(play.Pause().HasValue());
    REQUIRE(play.ReadDecisionProblem());
    play.Stop();
    CHECK_FALSE(play.ReadDecisionProblem());
    CHECK(captured->error.code.Value() == error.code.Value());
}

TEST_CASE("Play keeps bounded immutable agent inspection history for one live generation", "[editor][play][ai]") {
    using Controller = Editor::EditorPlaySessionController;
    const auto observations = std::make_shared<PlayOwnerObservations>();
    auto registry = Registry();
    Controller play;
    REQUIRE(play.Start(AuthoringScene(), registry, ObservePlayOwner(observations)).HasValue());
    const auto owner = play.SceneView()->Find({2});
    REQUIRE(owner);
    play.RefreshAgentInspection(owner);
    CHECK(play.AgentInspectionHistory().empty());

    constexpr auto spacing = Controller::AgentInspectionTickSpacing;
    constexpr auto limit = Controller::AgentInspectionHistoryLimit;
    for (std::uint64_t sample = 0; sample <= limit; ++sample) {
        observations->inspectionTick = 1 + sample * spacing;
        play.RefreshAgentInspection(owner);
        play.RefreshAgentInspection(owner);
        ++observations->inspectionTick;
        play.RefreshAgentInspection(owner);
    }
    REQUIRE(play.AgentInspectionHistory().size() == limit);
    CHECK(play.AgentInspectionHistory().front().tick == 1 + spacing);
    CHECK(observations->inspectionReads == limit + 1);

    REQUIRE(play.Pause().HasValue());
    observations->inspectionTick = 1 + (limit + 1) * spacing;
    ++observations->inspectionGeneration;
    play.RefreshAgentInspection(owner);
    REQUIRE(play.AgentInspectionHistory().size() == 1);
    CHECK(play.AgentInspectionHistory().front().agent.slot.generation == 2);

    auto stale = *owner;
    ++stale.entity.generation;
    play.RefreshAgentInspection(stale);
    CHECK(play.AgentInspectionHistory().empty());
    play.RefreshAgentInspection(owner);
    CHECK(play.AgentInspectionHistory().empty());
    CHECK(observations->inspectionReads == limit + 2);
    play.Stop();
    CHECK(play.AgentInspectionHistory().empty());
}

TEST_CASE("Play injected-owner failure preserves cause and closes the single Scene service", "[editor][play][ownership]") {
    auto observations = std::make_shared<PlayOwnerObservations>();
    observations->failTick = true;
    auto registry = Registry();
    Editor::EditorPlaySessionController play;
    REQUIRE(play.Start(AuthoringScene(), registry, ObservePlayOwner(observations)).HasValue());
    const auto advanced = play.FixedUpdate({}, {1.0 / 60.0});
    REQUIRE(advanced.HasError());
    CHECK(advanced.ErrorValue().code.Value() == Gameplay::GameplayErrors::PhysicsUnavailable.code.Value());
    CHECK(play.State() == Editor::EditorPlaySessionState::Failed);
    CHECK_FALSE(play.SceneView());
    CHECK(observations->closed == 1);
}

namespace {
    /** @brief Matches the real workspace boundary: Play errors stay local while the Editor host continues. */
    class PlayFixedParticipant final : public Runtime::RuntimeLifecycleParticipant {
    public:
        explicit PlayFixedParticipant(Editor::EditorPlaySessionController &play) : play_(play) {}

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {}

        Result<void> OnPhase(Runtime::RuntimePhase, const Runtime::FrameContext &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
            localFailure = play_.OnRuntimeFixedUpdate({}, context).HasError();
            return Result<void>::Success();
        }

        Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &context) override {
            localFailure = play_.PrepareRuntimeFixedCommit(context).HasError() || localFailure;
            return Result<void>::Success();
        }

        void CommitFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            play_.CommitRuntimeFixedUpdate(context);
        }

        void AbortFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            play_.AbortRuntimeFixedUpdate(context);
        }

        bool localFailure{};

    private:
        Editor::EditorPlaySessionController &play_;
    };

    class PlayFixedGate final : public Runtime::RuntimeLifecycleParticipant {
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
            return fail ? Result<void>::Failure(MakeError(Gameplay::GameplayErrors::PhysicsUnavailable)) : Result<void>::Success();
        }

        Runtime::FrameScheduler *scheduler{};
        bool fail{}, retire{};
    };

    struct PlayFixedFixture final {
        PlayFixedFixture() {
            auto made = Runtime::FrameScheduler::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
            REQUIRE(made.HasValue());
            scheduler = std::move(made).Value();
            observations->source = scheduler->DispatchSource();
            REQUIRE(play.BindRuntimeDispatch(observations->source).HasValue());
            REQUIRE(play.Start(AuthoringScene(), registry, ObservePlayOwner(observations)).HasValue());
            auto participant = std::make_unique<PlayFixedParticipant>(play);
            session = participant.get();
            REQUIRE(lifecycle.AddParticipant(std::move(participant)).HasValue());
            auto late = std::make_unique<PlayFixedGate>();
            gate = late.get();
            gate->scheduler = scheduler.get();
            REQUIRE(lifecycle.AddParticipant(std::move(late)).HasValue());
            REQUIRE(lifecycle.Startup(token).HasValue());
            REQUIRE(scheduler->RunFrame(lifecycle, token, false).HasValue());
        }

        Result<void> Tick() {
            clock.Advance(Duration::FromMilliseconds(10));
            return scheduler->RunFrame(lifecycle, token, false);
        }

        DeterministicClock clock;
        CancellationSource cancellation;
        CancellationToken token{cancellation.Token()};
        std::unique_ptr<Runtime::FrameScheduler> scheduler;
        std::shared_ptr<PlayOwnerObservations> observations{std::make_shared<PlayOwnerObservations>()};
        Gameplay::BehaviorRegistry registry{Registry()};
        Editor::EditorPlaySessionController play;
        Runtime::RuntimeLifecycle lifecycle;
        PlayFixedParticipant *session{};
        PlayFixedGate *gate{};
    };

    /** @brief A real registered native executor with a typed failure in the reactive condition callback. */
    class EditorDecisionFixtureExecutor final : public AI::IBehaviorTreeExecutor {
    public:
        enum class FailedCallback {
            Condition,
            Start,
            Resume
        };

        struct State final {
            std::atomic_bool admitted{true};
            std::uint64_t startTick{}, resumeTick{}, checkTick{};
            unsigned factories{};
            FailedCallback failure{FailedCallback::Condition};
        };

        explicit EditorDecisionFixtureExecutor(State &state) : state_(state) {}

        Result<AI::AiTaskState> Start(const AI::BehaviorTreeEvaluationContext &context,
                                      const AI::AiTaskOperationContext &) noexcept override {
            state_.startTick = context.tick;
            if (state_.failure == FailedCallback::Start)
                return Result<AI::AiTaskState>::Failure(MakeError(AI::AIErrors::TaskFailureInvalid));
            return Result<AI::AiTaskState>::Success(AI::AiTaskState::Running);
        }

        Result<AI::AiTaskState> Resume(const AI::BehaviorTreeEvaluationContext &context,
                                       const AI::AiTaskOperationContext &) noexcept override {
            state_.resumeTick = context.tick;
            if (state_.failure == FailedCallback::Resume)
                return Result<AI::AiTaskState>::Failure(MakeError(AI::AIErrors::TaskFailureInvalid));
            return Result<AI::AiTaskState>::Success(AI::AiTaskState::Running);
        }

        void Abort(const AI::AiTaskOperationContext &, AI::AiTaskCancellationReason) noexcept override {}

        void Cleanup(const AI::AiTaskOperationContext &, const AI::AiTaskTerminalResult &) noexcept override {}

        Result<bool> Check(const AI::BehaviorTreeEvaluationContext &context) noexcept override {
            state_.checkTick = context.tick;
            if (state_.failure == FailedCallback::Condition && context.tick == 2)
                return Result<bool>::Failure(MakeError(AI::AIErrors::TaskFailureInvalid));
            return Result<bool>::Success(true);
        }

        Result<void> Service(const AI::BehaviorTreeEvaluationContext &) noexcept override {
            return Result<void>::Success();
        }

    private:
        State &state_;
    };
}  // namespace

TEST_CASE("Cooked Editor Play executes real Decision scheduling and exposes only committed live failures", "[editor][play][ai][product]") {
    namespace Decisions = AI::DecisionGraphTestSupport;
    namespace Cook = Application::PrefabCookTestSupport;
    namespace Output = Assets::OutputTestSupport;
    using namespace Application::Internal;
    Cook::HostFixture project;
    Decisions::Fixture inputs;
    const auto providerState = std::make_shared<EditorDecisionFixtureExecutor::State>();
    std::array registrations{Decisions::Registration(inputs.probe), Decisions::Registration(inputs.probe, 502)};
    registrations[1].contract.role = AI::DecisionNodeRole::Condition;
    for (auto &registration : registrations) {
        registration.contract.memory.stateBytes = sizeof(EditorDecisionFixtureExecutor);
        registration.factory.userData = providerState.get();
        registration.factory.create = [](void *owner, std::span<const AI::BehaviorTreeProperty>) -> AI::IBehaviorTreeExecutor * {
            auto &state = *static_cast<EditorDecisionFixtureExecutor::State *>(owner);
            ++state.factories;
            return new EditorDecisionFixtureExecutor(state);
        };
    }
    auto providers = AI::DecisionNodeGeneration::Capture(registrations, {providerState, &providerState->admitted});
    REQUIRE(providers.HasValue());
    inputs.providers = std::move(providers).Value();
    auto condition = Decisions::TreeNode(2, AI::DecisionControlKind::Condition);
    condition.kind = AI::BehaviorTreeNodeKind::Decorator;
    condition.type = Decisions::MakeIdentity<AI::BehaviorTreeNodeTypeId>(502);
    condition.minimumChildren = 1;
    condition.maximumChildren = 1;
    condition.control.abortMode = AI::BehaviorTreeAbortMode::Self;
    auto graph = Decisions::Tree(84,
                                 {Decisions::TreeNode(1, AI::DecisionControlKind::Root), condition,
                                  Decisions::TreeNode(3, AI::DecisionControlKind::Task)},
                                 {Decisions::Edge(1, 1, 2), Decisions::Edge(2, 2, 3)});
    graph.executionTask = Decisions::MakeIdentity<AI::TaskId>(401);
    const auto decision = inputs.Compile(std::span{&graph, 1}, 84);
    const auto controller = Decisions::MakeIdentity<AI::ControllerTypeId>(402);
    const auto agent = Decisions::MakeIdentity<AI::AgentId>(403);
    const auto decisionId = Assets::AssetId::FromBytes({0x42});
    const auto schemaId = Assets::AssetId::FromBytes({0x43});
    constexpr auto sceneType = "core.scene";
    constexpr auto treeType = "core.behavior_tree";
    constexpr auto scenePath = "assets/level.horo";
    constexpr auto perceptionPath = "play-perception.json";
    bool livePerception = false;
    SECTION("Prepared native sensing reaches the selected agent's committed inspection memory") {
        livePerception = true;
    }
    SceneSource::SceneSourceDocument source;
    source.objects.push_back({.id = {7},
                              .name = "Owned AI and camera",
                              .components = {.camera = Runtime::CameraComponent{},
                                             .aiAgent = AI::AiAgentComponent{.agent = agent},
                                             .aiController = AI::AiControllerComponent{.controller = controller,
                                                                                       .decisionAsset = graph.asset,
                                                                                       .blackboardSchema = inputs.schema->Identity()}}});
    if (livePerception) {
        auto *configured = &*source.objects.front().components.aiController;
        configured->requiredCapabilities = AI::AiCapabilitySet::Of(AI::AiCapability::Perception);
        source.objects.push_back({.id = {8},
                                  .name = "Sensed peer",
                                  .localTransform = {.translation = {1.0F, 0.0F, 0.0F}},
                                  .components = {.aiAgent = AI::AiAgentComponent{.agent = Decisions::MakeIdentity<AI::AgentId>(404)},
                                                 .aiController = *configured}});
    }
    const SceneSource::SceneSourceView view{source.objects, source.prefabInstances};
    const auto runtime = SceneSource::ConvertSceneSourceToRuntime(view, SceneCook::SceneDefinitionIdForAsset(project.sceneId), {1});
    REQUIRE(runtime.HasValue());
    const auto scene = SceneCook::EncodeCookedSceneDefinition(runtime.Value());
    const auto plan = AI::DecisionPlanCodec::EncodeCooked(*decision);
    const auto schema = AI::BlackboardSchemaCodec::Encode(*inputs.schema);
    const auto authored = AI::DecisionPlanCodec::EncodeSource(std::span{&graph, 1}, graph.asset);
    REQUIRE(scene.HasValue());
    REQUIRE(plan.HasValue());
    REQUIRE(schema.HasValue());
    REQUIRE(authored.HasValue());
    std::filesystem::rename(project.project.dir.path / "assets/level.scene", project.project.dir.path / scenePath);
    std::filesystem::rename(project.project.dir.path / "assets/level.scene.horo", project.project.dir.path / "assets/level.horo.horo");
    project.WriteText(scenePath, SceneSource::EncodeSceneSource(view));
    project.WriteText("assets/check.horo_bt", {reinterpret_cast<const char *>(authored.Value().data()), authored.Value().size()});
    project.WriteText("assets/check.horo_bt.horo", Cook::SidecarJson(decisionId.ToString(), treeType));
    const auto schemaPath = std::string{"assets/keys"} + std::string{AI::BlackboardSchemaSourceExtension};
    project.WriteText(schemaPath, {reinterpret_cast<const char *>(schema.Value().data()), schema.Value().size()});
    project.WriteText(schemaPath + ".horo", Cook::SidecarJson(schemaId.ToString(), AI::BlackboardSchemaAssetType));
    const auto target = project.request.assets.target;
    std::vector<Assets::AssetCookManifestEntry> entries;
    std::vector<std::vector<std::uint8_t>> artifacts;
    const auto add = [&](const Assets::AssetId id, std::string_view type, const std::vector<std::uint8_t> &bytes) {
        const auto assetType = Cook::Type(type);
        auto encoded = Assets::EncodeCookedArtifact(
            {.id = id, .type = assetType, .target = target, .payloadDigest = Output::DigestOf(bytes), .payload = bytes});
        REQUIRE(encoded.HasValue());
        entries.push_back({id, assetType, id.ToString() + ".cooked", Output::DigestOf(encoded.Value())});
        artifacts.push_back(std::move(encoded).Value());
    };
    add(project.sceneId, sceneType, scene.Value());
    add(decisionId, treeType, plan.Value());
    add(schemaId, AI::BlackboardSchemaAssetType, schema.Value());
    REQUIRE(Output::PublishFixture(project.cooked.path, target, entries, artifacts).HasValue());

    bool changed = false;
    bool rejectFixed = false;
    bool discloseKey = false;
    bool authorizeForeignSchema = false;
    using FailedCallback = EditorDecisionFixtureExecutor::FailedCallback;
    SECTION("Committed pause and step retain a consecutive local Decision clock") {}
    SECTION("An explicit schema-scoped inspector key reveals only its admitted value") {
        discloseKey = true;
    }
    SECTION("An inspector key from another schema leaves this Blackboard redacted") {
        authorizeForeignSchema = true;
    }
    SECTION("Start errors reach Problems on the first committed tick") {
        providerState->failure = FailedCallback::Start;
    }
    SECTION("Resume errors reach Problems after a paused step") {
        providerState->failure = FailedCallback::Resume;
    }
    SECTION("An aborted step retires the evaluated owner before retry") {
        rejectFixed = true;
    }
    SECTION("A changed live Scene cannot admit stale cooked controllers") {
        changed = true;
    }
    if (changed)
        source.objects.front().localTransform.translation.x = 3;

    REQUIRE_FALSE(inputs.schema->Keys().empty());
    const auto &disclosed = inputs.schema->Keys().front();
    std::vector<EditorPlayInspectionKey> disclosure;
    if (discloseKey)
        disclosure.push_back({inputs.schema->Identity(), disclosed.key});
    if (authorizeForeignSchema)
        disclosure.push_back({Decisions::MakeIdentity<AI::BlackboardSchemaId>(inputs.schema->Identity().Value() + 1), disclosed.key});
    std::unique_ptr<Gameplay::LoadedGameModule> senseModule;
    std::shared_ptr<const AI::PerceptionSenseGeneration> senses;
    if (livePerception) {
        const Gameplay::GameModuleLoadExpectation expected{"game.ai.tests", Gameplay::CurrentGameplayBuildFingerprint(),
                                                           Horo::Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH)};
        auto loaded = Gameplay::GameModuleHost{}.Load(HORO_TEST_AI_MODULE_PATH, expected);
        REQUIRE(loaded.HasValue());
        REQUIRE_FALSE(loaded.Value().diagnostic);
        senseModule = std::move(loaded).Value().owner;
        auto generation = senseModule->AcquirePerceptionSenses();
        REQUIRE(generation.HasValue());
        senses = std::move(generation).Value();
        REQUIRE(senses->Contracts().size() == 1);
        const auto &contract = senses->Contracts().front();
        const auto sense = std::to_string(contract.sense.identity.Value());
        const auto units = (contract.workUnits + AI::PerceptionSenseGeneration::ValidationWorkUnits + 1) * 3;
        project.WriteText(perceptionPath,
                          "{\"schema\":2,\"sceneAsset\":\"" + project.sceneId.ToString() +
                              "\",\"sceneRevision\":1,\"worldOriginMillimeters\":[0,0,0],\"controllers\":[{\"controller\":" +
                              std::to_string(controller.Value()) + ",\"listeners\":[91],\"sources\":[" + sense +
                              "],\"layers\":1,\"mask\":1}],\"native\":{\"listeners\":[{\"id\":91,\"sense\":" + sense +
                              "}],\"radiusMillimeters\":4000,\"visibleLayers\":1,\"workUnitsPerTick\":" + std::to_string(units) + "}}");
    }
    const auto projectFactory =
        MakeEditorProjectSceneFactory(project.jobs, {.cooked = SceneCookSelection{project.cooked.path, target},
                                                     .decisions = EditorPlayDecisionSelection{project.project.dir.path,
                                                                                              project.sceneId,
                                                                                              {{controller, decisionId, {}}},
                                                                                              std::move(disclosure),
                                                                                              livePerception ? perceptionPath : "",
                                                                                              Duration::FromMilliseconds(10)}});
    const Editor::EditorPlaySceneFactory factory = [&](Runtime::SceneRuntimeId id, const Runtime::RuntimeSceneDefinition &definition) {
        return projectFactory(project.project.dir.path, id, definition);
    };
    Gameplay::BehaviorRegistry registry;
    Gameplay::ComponentRegistry components;
    REQUIRE(registry.Freeze().HasValue());
    REQUIRE(components.Freeze().HasValue());
    Editor::EditorPlaySessionController play;
    DeterministicClock clock;
    auto scheduler = Runtime::FrameScheduler::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
    REQUIRE(scheduler.HasValue());
    REQUIRE(play.BindRuntimeDispatch(scheduler.Value()->DispatchSource()).HasValue());
    const Editor::SceneDocumentSnapshot document{{1}, {1}, source.objects};
    const auto started = play.Start(document, registry, components, factory, {}, Editor::EditorPlayViewSource::AuthoredCamera, nullptr, {},
                                    nullptr, {.decision = [&] {
        return Result<std::shared_ptr<const AI::DecisionNodeGeneration>>::Success(inputs.providers);
    }, .perception = [&] {
        return Result<std::shared_ptr<const AI::PerceptionSenseGeneration>>::Success(senses);
    }});
    if (changed) {
        REQUIRE(started.HasError());
        CHECK(providerState->factories == 0);
        CHECK_FALSE(play.SceneView());
        return;
    }
    if (started.HasError())
        UNSCOPED_INFO(started.ErrorValue().message);
    REQUIRE(started.HasValue());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (play.State() == Editor::EditorPlaySessionState::Starting && std::chrono::steady_clock::now() < deadline) {
        play.PresentationUpdate({0});
        std::this_thread::yield();
    }
    if (play.LastError())
        UNSCOPED_INFO(play.LastError()->message);
    REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
    REQUIRE(play.SceneView());
    const auto entity = play.SceneView()->Find({7});
    REQUIRE(entity);
    play.RefreshAgentInspection(entity);
    CHECK(play.AgentInspectionHistory().empty());
    CHECK(play.QuiesceForReload().HasError());
    Runtime::RuntimeLifecycle lifecycle;
    REQUIRE(lifecycle.AddParticipant(std::make_unique<PlayFixedParticipant>(play)).HasValue());
    auto gate = std::make_unique<PlayFixedGate>();
    auto *gateView = gate.get();
    REQUIRE(lifecycle.AddParticipant(std::move(gate)).HasValue());
    const CancellationSource cancellation;
    REQUIRE(lifecycle.Startup(cancellation.Token()).HasValue());
    REQUIRE(scheduler.Value()->RunFrame(lifecycle, cancellation.Token(), false).HasValue());
    const auto tick = [&] {
        clock.Advance(Duration::FromMilliseconds(10));
        return scheduler.Value()->RunFrame(lifecycle, cancellation.Token(), false);
    };
    const auto verifyProblem = [&](const std::uint64_t expectedTick) {
        const auto problem = play.ReadDecisionProblem();
        REQUIRE(problem);
        CHECK(problem->tick == expectedTick);
        CHECK(problem->agent == agent);
        CHECK(problem->entity == entity);
        const auto &failure = AI::AIErrors::TaskFailureInvalid;
        CHECK(ErrorChainContains(problem->error, failure.domain, failure.code));
        if (providerState->failure == FailedCallback::Condition) {
            CHECK(problem->phase == Editor::EditorPlayDecisionPhase::Evaluate);
        } else {
            CHECK(problem->phase == Editor::EditorPlayDecisionPhase::TaskProvider);
            CHECK(problem->graph == graph.asset);
            CHECK(problem->node == Decisions::MakeIdentity<AI::DecisionNodeId>(3));
            CHECK(play.SceneView());
        }
    };
    REQUIRE(tick().HasValue());
    CHECK(providerState->startTick == 1);
    play.RefreshAgentInspection(entity);
    REQUIRE(play.AgentInspectionHistory().size() == 1);
    const auto &inspection = play.AgentInspectionHistory().front();
    CHECK(inspection.owner == entity);
    CHECK(inspection.tick == 1);
    REQUIRE(inspection.snapshot);
    REQUIRE(inspection.decisionTrace);
    CHECK(inspection.decisionTrace->Header().tick == 1);
    CHECK(inspection.decisionTrace->Header().root == graph.asset);
    CHECK(inspection.decisionTrace->Header().blackboard.agent == inspection.agent);
    CHECK(inspection.snapshot->agent == inspection.agent);
    CHECK(inspection.snapshot->decisionTick == 1);
    if (livePerception) {
        CHECK_FALSE(inspection.snapshot->perceptionFailure);
        REQUIRE(inspection.snapshot->stimuli.size() == 1);
        const auto &stimulus = inspection.snapshot->stimuli.front();
        REQUIRE(play.SceneView()->Find({8}));
        CHECK(stimulus.key.source.slot == play.SceneView()->Find({8})->entity.index);
        CHECK(stimulus.key.sense == senses->Contracts().front().sense.identity);
        CHECK(stimulus.firstSensedTick == 1);
        CHECK(stimulus.lastSensedTick == 1);
        CHECK(stimulus.ageSeconds == 0);
        CHECK(stimulus.sensed);
        play.Stop();
        lifecycle.Shutdown();
        return;
    }
    CHECK(inspection.snapshot->omittedDecisionTasks == 0);
    if (providerState->failure == FailedCallback::Start) {
        CHECK(inspection.snapshot->decisionTasks.empty());
    } else {
        REQUIRE(inspection.snapshot->decisionTasks.size() == 1);
        const auto &task = inspection.snapshot->decisionTasks.front();
        CHECK(task.occurrence.graph == graph.asset);
        CHECK(task.node == Decisions::MakeIdentity<AI::DecisionNodeId>(3));
        CHECK(task.task.IsValid());
        CHECK(task.execution.IsValid());
    }
    for (const auto &key : inspection.snapshot->keys) {
        if (discloseKey && key.key == disclosed.key) {
            CHECK(key.status == (disclosed.defaultValue ? AI::AiInspectionValueStatus::Present : AI::AiInspectionValueStatus::Unset));
            CHECK(key.value == disclosed.defaultValue);
        } else {
            CHECK(key.status == AI::AiInspectionValueStatus::Redacted);
            CHECK_FALSE(key.value);
        }
    }
    if (providerState->failure == FailedCallback::Start) {
        verifyProblem(1);
        play.Stop();
        CHECK_FALSE(play.ReadDecisionProblem());
        lifecycle.Shutdown();
        return;
    }
    CHECK_FALSE(play.ReadDecisionProblem());
    REQUIRE(play.Pause().HasValue());
    REQUIRE(tick().HasValue());
    REQUIRE(tick().HasValue());
    CHECK(providerState->checkTick == 1);
    REQUIRE(play.Step().HasValue());
    gateView->fail = rejectFixed;
    if (rejectFixed) {
        REQUIRE(tick().HasError());
        CHECK(providerState->checkTick == 2);
        CHECK_FALSE(play.SceneView());
        CHECK_FALSE(play.ReadDecisionProblem());
    } else {
        REQUIRE(tick().HasValue());
        CHECK(providerState->checkTick == 2);
        if (providerState->failure == FailedCallback::Resume)
            CHECK(providerState->resumeTick == 2);
        verifyProblem(2);
    }
    play.Stop();
    CHECK_FALSE(play.ReadDecisionProblem());
    lifecycle.Shutdown();
}

TEST_CASE("Play typed fixed path publishes only after actual same-attempt commitment", "[editor][play][fixed-commit]") {
    PlayFixedFixture fixture;
    REQUIRE(fixture.Tick().HasValue());
    const auto &seen = *fixture.observations;
    CHECK(seen.ticks == 1);
    CHECK(seen.prepares == 1);
    CHECK(seen.publications == 1);
    CHECK(seen.aborts == 0);
    CHECK_FALSE(seen.pending);
    CHECK(seen.prepareFacts.committedTick == 0);
    CHECK(seen.commitFacts.committedTick == seen.updateFacts.fixedTick);
    CHECK(seen.commitFacts.committedAttempt == seen.updateFacts.fixedAttempt);
    CHECK(seen.closed == 0);
    CHECK_FALSE(fixture.session->localFailure);
}

TEST_CASE("Play retains a paused step across global rollback and consumes it only at commit", "[editor][play][fixed-commit]") {
    PlayFixedFixture fixture;
    REQUIRE(fixture.play.Pause().HasValue());
    REQUIRE(fixture.play.Step().HasValue());
    fixture.gate->fail = true;
    REQUIRE(fixture.Tick().HasError());
    CHECK(fixture.observations->aborts == 1);
    CHECK(fixture.observations->publications == 0);
    CHECK(fixture.play.State() == Editor::EditorPlaySessionState::Paused);
    fixture.gate->fail = false;
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
    CHECK(fixture.observations->ticks == 2);
    CHECK(fixture.observations->publications == 1);
    REQUIRE(fixture.Tick().HasValue());
    CHECK(fixture.observations->ticks == 2);
}

TEST_CASE("Play rollback reaches the retained Scene owner after pre-fence retirement", "[editor][play][fixed-commit]") {
    PlayFixedFixture fixture;
    fixture.gate->retire = true;
    REQUIRE(fixture.Tick().HasError());
    CHECK(fixture.observations->aborts == 1);
    CHECK(fixture.observations->abortStatus == Runtime::RuntimeDispatchStatus::Retired);
    CHECK_FALSE(fixture.observations->pending);
    CHECK(fixture.observations->closed == 0);
}

TEST_CASE("Play native or preparation failure aborts locally before closing its Scene", "[editor][play][fixed-commit]") {
    PlayFixedFixture fixture;
    SECTION("native update") {
        fixture.observations->failTick = true;
    }
    SECTION("preparation") {
        fixture.observations->failPrepare = true;
    }
    REQUIRE(fixture.Tick().HasValue());
    CHECK(fixture.session->localFailure);
    CHECK(fixture.play.State() == Editor::EditorPlaySessionState::Failed);
    CHECK(fixture.observations->aborts == 1);
    CHECK(fixture.observations->publications == 0);
    CHECK(fixture.observations->closed == 1);
    CHECK(fixture.scheduler->Statistics().completedSimulationTick == 1);
}

TEST_CASE("Play Stop inside a fixed callback retains its owner until safe rollback", "[editor][play][fixed-commit]") {
    PlayFixedFixture fixture;
    SECTION("native update") {
        fixture.observations->stopDuringUpdate = &fixture.play;
    }
    SECTION("preparation") {
        fixture.observations->stopDuringPrepare = &fixture.play;
    }
    REQUIRE(fixture.Tick().HasValue());
    CHECK(fixture.observations->stopRetainedOwner);
    CHECK(fixture.observations->aborts == 1);
    CHECK(fixture.observations->publications == 0);
    CHECK(fixture.observations->closed == 1);
    CHECK(fixture.play.State() == Editor::EditorPlaySessionState::Idle);
}

TEST_CASE("Play rejects fabricated stale and replacement issuers before mutation", "[editor][play][fixed-commit]") {
    PlayFixedFixture fixture;
    CHECK(fixture.play.BindRuntimeDispatch(fixture.observations->source).HasError());
    CHECK(fixture.play.FixedUpdate({}, Gameplay::FixedDeltaTime{0.01}).HasError());
    Runtime::FixedStepContext forged{.simulationTick = 1, .fixedDelta = Duration::FromMilliseconds(10), .cancellation = fixture.token};
    CHECK(fixture.play.OnRuntimeFixedUpdate({}, forged).HasError());
    CHECK(fixture.observations->ticks == 0);
    REQUIRE(fixture.Tick().HasValue());
    forged.dispatchEvidence = fixture.observations->evidence;
    CHECK(fixture.play.OnRuntimeFixedUpdate({}, forged).HasError());
    CHECK(fixture.observations->ticks == 1);
}

TEST_CASE("Play checks actual proof before idle preparation and foreign-thread rollback", "[editor][play][fixed-commit]") {
    PlayFixedFixture fixture;
    REQUIRE(fixture.Tick().HasValue());
    const auto &facts = fixture.observations->updateFacts;
    const Runtime::FixedStepContext expired{.simulationTick = facts.fixedTick,
                                            .fixedDelta = facts.fixedDuration,
                                            .cancellation = fixture.token,
                                            .attemptNumber = facts.fixedAttempt,
                                            .frameNumber = facts.frame,
                                            .dispatchEvidence = fixture.observations->evidence};
    CHECK(fixture.play.PrepareRuntimeFixedCommit(expired).HasError());
    bool rejected = false;
    std::thread foreign([&] {
        rejected = fixture.play.PrepareRuntimeFixedCommit(expired).HasError();
        fixture.play.AbortRuntimeFixedUpdate(expired);
    });
    foreign.join();
    CHECK(rejected);
    CHECK(fixture.observations->publications == 1);
    CHECK(fixture.observations->aborts == 0);
    CHECK(fixture.observations->closed == 0);
}
