#pragma once

#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "GameplayWorldComposition.h"
#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <cmath>

namespace Horo::Tests::CharacterIntegration {
    using namespace Horo::Application::Internal;
    using namespace Horo::Character;
    using namespace Horo::Gameplay;
    using namespace Horo::Physics;

    struct CanonicalCharacterHost final {
        std::unique_ptr<PhysicsRuntime> runtime;
        std::unique_ptr<Runtime::RuntimeScene> scene;
        std::unique_ptr<PhysicsWorld> physics;
        std::unique_ptr<GameplayWorldComposition> gameplay;
        std::unique_ptr<CharacterWorld> character;
        CharacterControllerDescriptor descriptor;
        CharacterControllerHandle controller;
        PhysicsQueryFixture ramp;
        Math::Vec3 normal;

        explicit CanonicalCharacterHost(const float degrees = 0.0F,
                                        const CharacterSteepSlopePolicy policy = CharacterSteepSlopePolicy::Stop) {
            auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(created.HasValue());
            runtime = std::move(created).Value();
            const float radians = degrees * Math::Pi / 180.0F;
            normal = {-std::sin(radians), std::cos(radians), 0};
            const auto definition =
                Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, BehaviorTypeId::Parse("game.tests.dynamic_mover").Value());
            scene = Runtime::RuntimeScene::Create(definition, {7}).Value();
            physics = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
            REQUIRE(physics->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
            REQUIRE(
                physics->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
            const GameplayWorldSelection selection{.moduleId = "game.tests",
                                                   .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                                   .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                                   .physicsPermission = GameplayPhysicsPermission::Granted};
            gameplay = GameplayWorldComposition::Create(*scene, physics.get(), 1, selection).Value();
            CharacterWorldSettingsDescriptor settings;
            settings.capacities.maximumControllers = 1;
            character = CharacterWorld::Prepare({1, physics->Identity(), 1, 1}, CharacterWorldSettings::Capture(settings).Value()).Value();
            descriptor.sceneGeneration = 1;
            descriptor.characterWorld = character->Descriptor().identity;
            descriptor.physicsWorld = physics->Identity();
            descriptor.capsule = {0.25F, 0.5F};
            descriptor.collisionRootPosition = {0,
                                                descriptor.capsule.cylindricalHalfHeightMeters +
                                                    descriptor.capsule.radiusMeters / normal.y + descriptor.skinWidthMeters,
                                                0};
            descriptor.steepSlopePolicy = policy;
            descriptor.collisionProfile = CollisionProfileId::Parse("22345678-1234-4234-8234-123456789abc").Value();
            descriptor.queryChannel = PhysicsQueryChannelId::Parse("32345678-1234-4234-8234-123456789abc").Value();
            descriptor.defaultMaterial = {Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(), 1,
                                          PhysicsMaterialSlotId::FromValue(1)};
            descriptor.selectors.requiredLayer = CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value();
            const PhysicsQueryFixtureDescriptor plane{.shape = PhysicsStaticPlaneShape{normal, 0},
                                                      .layer = *descriptor.selectors.requiredLayer,
                                                      .profile = descriptor.collisionProfile,
                                                      .channel = descriptor.queryChannel};
            const auto added = physics->CreateQueryFixture(plane);
            REQUIRE(added.HasValue());
            ramp = added.Value();
            controller = character->CreateController(descriptor).Value();
            REQUIRE(character->Activate().HasValue());
            CharacterPhysicsQueryAdapter adapter{*physics};
            REQUIRE(character->SpawnController(controller, adapter.Context(Expectations(0))).HasValue());
        }

        ~CanonicalCharacterHost() {
            if (gameplay)
                gameplay->Shutdown();
            if (character)
                character->Shutdown();
            if (physics)
                physics->Shutdown();
        }

        [[nodiscard]] CharacterPhysicsQueryExpectations Expectations(const std::uint64_t tick) const {
            const auto &owner = character->Descriptor();
            return {owner.sceneGeneration,  owner.identity, owner.physicsWorld,           owner.collisionFilterGeneration,
                    owner.originGeneration, tick,           owner.physicsSnapshotRevision};
        }

        [[nodiscard]] CharacterLocomotionSnapshot Move(const std::uint64_t tick, const Math::Vec3 velocity,
                                                       const std::int64_t nanoseconds = 16'666'667, const bool jump = false) {
            REQUIRE(character
                        ->QueueMovementCommand({.controller = controller,
                                                .tick = tick,
                                                .sequence = tick,
                                                .desiredVelocityMetersPerSecond = velocity,
                                                .jumpRequested = jump})
                        .HasValue());
            CharacterPhysicsQueryAdapter adapter{*physics};
            REQUIRE(character
                        ->AdvanceFixedTick({.tick = tick,
                                            .sceneGeneration = 1,
                                            .fixedDelta = Duration::FromNanoseconds(nanoseconds),
                                            .query = adapter.Context(Expectations(tick))})
                        .HasValue());
            const auto snapshot = character->ControllerLocomotionSnapshot(controller).Value();
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasValue());
            Runtime::SceneCommandBuffer commands;
            Math::Transform transform;
            transform.translation = snapshot.transform.position;
            const auto entity = *scene->View().Find({1});
            commands.SetLocalTransform(entity, transform);
            REQUIRE(scene->Commit(commands).HasValue());
            REQUIRE(scene->View().Get(entity).Value().localTransform->translation == snapshot.transform.position);
            return snapshot;
        }
    };

}  // namespace Horo::Tests::CharacterIntegration
