#include "Horo/Gameplay/GameplayErrors.h"
#include "editor/gameplay/EditorPlaySessionController.h"

#include <catch2/catch_test_macros.hpp>

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
}  // namespace

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
    auto actor = play.Scene()->View().Find(Runtime::SceneObjectId{2});
    REQUIRE(actor.has_value());
    REQUIRE(play.Scene()->View().Get(*actor).Value().localTransform->translation.x == 1.0F);

    REQUIRE(play.Step().HasValue());
    REQUIRE(play.FixedUpdate({}, Gameplay::FixedDeltaTime{1.0 / 60.0}).HasValue());
    REQUIRE(play.Scene()->View().Get(*actor).Value().localTransform->translation.x == 2.0F);
    REQUIRE(authoring.objects[1].localTransform.translation.x == 0.0F);

    play.Stop();
    REQUIRE(play.State() == Editor::EditorPlaySessionState::Idle);
    REQUIRE(play.Scene() == nullptr);
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
    const Runtime::SceneRuntimeId runtimeId = play.Scene()->View().RuntimeId();
    REQUIRE(play.FixedUpdate({}, Gameplay::FixedDeltaTime{1.0 / 60.0}).HasValue());

    REQUIRE(play.ReloadBehaviors(candidate, original).HasValue());
    REQUIRE(play.Scene()->View().RuntimeId() == runtimeId);
    REQUIRE(play.FixedUpdate({}, Gameplay::FixedDeltaTime{1.0 / 60.0}).HasValue());
    const auto actor = play.Scene()->View().Find(Runtime::SceneObjectId{2});
    REQUIRE(actor.has_value());
    REQUIRE(play.Scene()->View().Get(*actor).Value().localTransform->translation.x == 3.0F);
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
    REQUIRE(play.Scene()->View().Find(Runtime::SceneObjectId{2}).has_value());
}
