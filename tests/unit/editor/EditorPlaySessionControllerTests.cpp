#include "EditorPlayXrHost.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "editor/document/EditorAssetMeshCache.h"
#include "editor/gameplay/EditorPlaySessionController.h"
#include "editor/renderer/xr/EditorXrSceneGeometry.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
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

        Result<void> UpdateScene(Runtime::RuntimeSceneView) override {
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
                const auto candidate = scene;
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

        std::unique_ptr<Runtime::SceneActivationParticipant> MakeSceneParticipant() override {
            return std::make_unique<Participant>(*this);
        }

    private:
        Editor::EditorXrSceneSourcePort source_;
    };

    struct PlayOwnerObservations final {
        std::vector<Runtime::SceneRuntimeId> admitted;
        std::size_t ticks{};
        std::size_t closed{};
        bool failTick{};
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
