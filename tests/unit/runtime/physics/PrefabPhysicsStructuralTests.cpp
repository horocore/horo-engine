#include "PhysicsSceneActivationTestSupport.h"

namespace Horo::Physics {
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        /** @brief Owns the real selected Physics world and complete detached Scene projection. */
        struct StructuralFixture {
            std::unique_ptr<PhysicsRuntime> runtime{PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value()};
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant activation{*runtime, authority, Settings()};
            std::unique_ptr<Runtime::SceneStructuralParticipant> participant{activation.MakeStructuralParticipant()};
            std::unique_ptr<Runtime::RuntimeScene> scene;
            std::unique_ptr<Runtime::SceneActivationCandidate> owner;
            PhysicsSceneActivationCandidate *physics{};
            Runtime::RuntimeComponentSet components;
            Runtime::EntityRef entity;
            Math::Transform transform;
            std::array<Runtime::ResolvedGroupPhysicsBodyReference, 1> references;

            StructuralFixture() {
                const auto definition = Definition();
                scene = RequireScene(definition);
                auto prepared = activation.Prepare(definition, scene->View());
                REQUIRE(prepared.HasValue());
                owner = std::move(prepared).Value();
                physics = dynamic_cast<PhysicsSceneActivationCandidate *>(owner.get());
                REQUIRE(physics != nullptr);
                REQUIRE(owner->ValidatePublication().HasValue());
                owner->Publish();

                components.rigidBody = Runtime::RigidBodyComponent{.id = {10}, .body = {100}};
                components.rigidBody->motion = Runtime::AuthoredPhysicsMotionType::Dynamic;
                components.rigidBody->mass = Runtime::AuthoredPhysicsMass{1.0F};
                components.colliders.push_back({.id = {20}, .collider = {200}, .body = {.body = {100}}, .collisionProfile = Profile()});
                entity = {scene->View().RuntimeId(), {1, 1}};
                references[0] = {Runtime::GroupPhysicsReferenceKind::ColliderBody, 0, entity, {100}};
            }

            auto PrepareAddition() {
                const std::array created{Runtime::RuntimeEntityView{.entity = entity,
                                                                    .localTransform = &transform,
                                                                    .components = &components,
                                                                    .physicsReferences = references}};
                return participant->Prepare(scene->View(), created, {});
            }
        };
    }  // namespace

    TEST_CASE("Physics structural owner publishes and retires exact runtime bindings after detached rollback",
              "[physics][prefab][structural]") {
        StructuralFixture fixture;
        auto &scene = fixture.scene;
        auto &participant = fixture.participant;
        auto *physics = fixture.physics;
        const auto entity = fixture.entity;
        auto staged = fixture.PrepareAddition();
        if (staged.HasError()) {
            INFO("Structural staging failure domain=" << staged.ErrorValue().domain.Value() << " code=" << staged.ErrorValue().code.Value()
                                                      << " message=" << staged.ErrorValue().message);
            REQUIRE(staged.HasValue());
        }
        REQUIRE(staged.HasValue());
        auto candidate = std::move(staged).Value();
        REQUIRE(candidate->ValidatePublication().HasValue());
        CHECK_FALSE(physics->FindRuntimeBody(entity, {100}).has_value());
        candidate.reset();
        CHECK_FALSE(physics->FindRuntimeShape(entity, {200}).has_value());
        staged = fixture.PrepareAddition();
        REQUIRE(staged.HasValue());
        candidate = std::move(staged).Value();
        REQUIRE(candidate->ValidatePublication().HasValue());
        Runtime::SceneCommandBuffer commands;
        (void)commands.Create({.components = fixture.components});
        REQUIRE(scene->Commit(commands).Value().created.front().entity == entity);
        candidate->Publish();
        REQUIRE(candidate->AfterPublication().HasValue());
        REQUIRE(physics->FindRuntimeBody(entity, {100}).has_value());
        REQUIRE(physics->FindRuntimeShape(entity, {200}).has_value());
        candidate.reset();
        staged = participant->Prepare(scene->View(), {}, std::array{entity});
        REQUIRE(staged.HasValue());
        candidate = std::move(staged).Value();
        CHECK(physics->FindRuntimeBody(entity, {100}).has_value());
        candidate.reset();
        CHECK(physics->FindRuntimeBody(entity, {100}).has_value());
        staged = participant->Prepare(scene->View(), {}, std::array{entity});
        REQUIRE(staged.HasValue());
        candidate = std::move(staged).Value();
        REQUIRE(candidate->ValidatePublication().HasValue());
        Runtime::SceneCommandBuffer removal;
        removal.Destroy(entity);
        REQUIRE(scene->Commit(removal).HasValue());
        candidate->Publish();
        CHECK_FALSE(physics->FindRuntimeBody(entity, {100}).has_value());
        CHECK_FALSE(physics->FindRuntimeShape(entity, {200}).has_value());
        fixture.owner->Shutdown();
        CHECK(participant->Prepare(scene->View(), {}, {}).HasError());
    }
#endif
}  // namespace Horo::Physics
