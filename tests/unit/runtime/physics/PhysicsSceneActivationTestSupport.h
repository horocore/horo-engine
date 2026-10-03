#pragma once

/** @file PhysicsSceneActivationTestSupport.h
 * @brief Shared authored-scene and asset fixtures for activation and containment regressions.
 */

#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsSceneActivation.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace {
        using namespace Assets;

        /** @brief Builds and validates an authored scene definition shared by fixture variants. */
        [[nodiscard]] Runtime::RuntimeSceneDefinition RequireDefinition(Runtime::SceneDefinitionBuilder &&builder) {
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
        }

        [[nodiscard]] Runtime::RuntimeSceneDefinition Definition(const std::uint64_t revision = 1) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{revision}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{1};
            builder.Add(std::move(entity));
            return RequireDefinition(std::move(builder));
        }

        [[nodiscard]] Assets::AssetId Asset(const std::uint8_t suffix) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = suffix;
            return Assets::AssetId::FromBytes(bytes);
        }

        [[nodiscard]] Assets::AssetTypeId AssetType() {
            return Assets::AssetTypeId::Parse("core.physics_material").Value();
        }

        [[nodiscard]] Physics::CollisionProfileId Profile() {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = 1;
            return Physics::CollisionProfileId::FromBytes(bytes);
        }

        [[nodiscard]] Assets::AssetRecord Record(const Assets::AssetId id, const Assets::AssetTypeId type) {
            const std::string name = id.ToString();
            const auto source = ProjectPath::Parse("assets/" + name + ".bin");
            const auto metadata = ProjectPath::Parse("assets/" + name + ".bin.horo");
            REQUIRE(source.HasValue());
            REQUIRE(metadata.HasValue());
            return {id, type, source.Value(), metadata.Value()};
        }

        [[nodiscard]] Runtime::RuntimeSceneDefinition PhysicsDefinition(const Assets::AssetId material,
                                                                        const Assets::AssetTypeId materialType,
                                                                        const std::size_t bodyCount = 2, const bool addConstraint = true,
                                                                        const std::uint64_t revision = 1,
                                                                        const bool declareDependency = true) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{revision}};
            for (std::size_t index = 0; index < bodyCount; ++index) {
                const std::uint64_t object = index + 1;
                const std::uint64_t bodySlot = 100 + index;
                Runtime::RigidBodyComponent body{.id = {10 + index}, .body = {bodySlot}};
                if (addConstraint && index == 0) {
                    body.motion = Runtime::AuthoredPhysicsMotionType::Dynamic;
                    body.mass = Runtime::AuthoredPhysicsMass{1.0F};
                }

                Runtime::ColliderComponent collider{
                    .id = {20 + index},
                    .collider = {200 + index},
                    .body = {.object = {object}, .body = {bodySlot}},
                    .collisionProfile = Profile(),
                    .materials = {{.slot = PhysicsMaterialSlotId::FromValue(1), .material = material}},
                };
                Runtime::RuntimeEntityDefinition entity{
                    .object = {object},
                    .components = {.rigidBody = body, .colliders = {collider}},
                };
                if (addConstraint && index == 0) {
                    Runtime::PhysicsConstraintComponent constraint{
                        .id = {50},
                        .constraint = {500},
                        .first = {.body = {.object = {1}, .body = {100}}},
                        .second = Runtime::PhysicsConstraintBodyEndpoint{.body = {.object = {2}, .body = {101}}},
                        .parameters = Runtime::PhysicsFixedConstraint{},
                    };
                    entity.components.physicsConstraints.push_back(std::move(constraint));
                }
                builder.Add(std::move(entity));
            }
            if (declareDependency)
                REQUIRE(builder.RequireAsset({material, materialType}).HasValue());
            return RequireDefinition(std::move(builder));
        }

        [[nodiscard]] Runtime::ColliderComponent Collider(const std::uint64_t component, const std::uint64_t slot,
                                                          const std::uint64_t object, const std::uint64_t body,
                                                          const Assets::AssetId material, Runtime::PhysicsColliderSource source,
                                                          const bool sensor = false) {
            return {.id = {component},
                    .collider = {slot},
                    .body = {.object = {object}, .body = {body}},
                    .source = std::move(source),
                    .collisionProfile = Profile(),
                    .materials = {{.slot = PhysicsMaterialSlotId::FromValue(1), .material = material}},
                    .sensor = sensor};
        }

        void AddFirstAnalyticEntity(Runtime::SceneDefinitionBuilder &builder, const Assets::AssetId material) {
            Runtime::RigidBodyComponent body{.id = {10}, .body = {100}};
            body.motion = Runtime::AuthoredPhysicsMotionType::Dynamic;
            body.mass = Runtime::AuthoredPhysicsDensity{500.0F};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = {1};
            entity.components.rigidBody = body;
            entity.components.colliders = {Collider(20, 200, 1, 100, material,
                                                    Runtime::PhysicsColliderSource{
                                                        Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{0.6F}}}),
                                           Collider(21, 201, 1, 100, material,
                                                    Runtime::PhysicsColliderSource{Runtime::PhysicsAnalyticCollider{
                                                        Runtime::PhysicsBoxCollider{{0.5F, 0.25F, 0.5F}}}})};
            Runtime::PhysicsConstraintComponent constraint;
            constraint.id = {50};
            constraint.constraint = {500};
            constraint.first.body = {.object = {1}, .body = {100}};
            constraint.second = Runtime::PhysicsConstraintBodyEndpoint{.body = {.object = {2}, .body = {101}},
                                                                       .localFrame = {.translation = {0.0F, 0.5F, 0.0F}}};
            constraint.parameters = Runtime::PhysicsFixedConstraint{};
            entity.components.physicsConstraints.push_back(std::move(constraint));
            builder.Add(std::move(entity));
        }

        void AddSecondAnalyticEntity(Runtime::SceneDefinitionBuilder &builder, const Assets::AssetId material) {
            Runtime::RuntimeEntityDefinition entity;
            entity.object = {2};
            entity.parent = Runtime::SceneObjectId{1};
            entity.localTransform = Math::Transform{.translation = {0.0F, 2.0F, 0.0F}};
            entity.components.rigidBody = Runtime::RigidBodyComponent{.id = {11}, .body = {101}};
            entity.components.colliders = {
                Collider(22, 202, 2, 101, material,
                         Runtime::PhysicsColliderSource{Runtime::PhysicsAnalyticCollider{Runtime::PhysicsCapsuleCollider{0.4F, 0.8F}}})};
            Runtime::PhysicsConstraintComponent constraint;
            constraint.id = {51};
            constraint.constraint = {501};
            constraint.first.body = {.object = {2}, .body = {101}};
            constraint.second = Runtime::PhysicsConstraintWorldEndpoint{.frame = {.translation = {0.0F, 0.0F, 0.0F}}};
            constraint.parameters = Runtime::PhysicsDistanceConstraint{.minimumMeters = 0.0F, .maximumMeters = 4.0F};
            entity.components.physicsConstraints.push_back(std::move(constraint));
            builder.Add(std::move(entity));
        }

        void AddThirdAnalyticEntity(Runtime::SceneDefinitionBuilder &builder, const Assets::AssetId material) {
            Runtime::RuntimeEntityDefinition entity;
            entity.object = {3};
            entity.localTransform = Math::Transform{.translation = {0.0F, -2.0F, 0.0F}};
            entity.components.rigidBody = Runtime::RigidBodyComponent{.id = {12}, .body = {102}};
            entity.components.colliders = {Collider(23, 203, 3, 102, material,
                                                    Runtime::PhysicsColliderSource{Runtime::PhysicsAnalyticCollider{
                                                        Runtime::PhysicsStaticPlaneCollider{{0.0F, 1.0F, 0.0F}, 0.0F}}})};
            builder.Add(std::move(entity));
        }

        [[nodiscard]] Runtime::RuntimeSceneDefinition AnalyticVariantDefinition(const Assets::AssetId material,
                                                                                const Assets::AssetTypeId materialType) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{4}};
            AddFirstAnalyticEntity(builder, material);
            AddSecondAnalyticEntity(builder, material);
            AddThirdAnalyticEntity(builder, material);
            REQUIRE(builder.RequireAsset({material, materialType}).HasValue());
            return RequireDefinition(std::move(builder));
        }

        [[nodiscard]] PhysicsSceneActivationSettings Settings(const std::uint32_t maximumBodies = 16,
                                                              const PhysicsNonFinitePolicy policy = PhysicsNonFinitePolicy::FailWorld) {
            PhysicsWorldSettingsDescriptor physicsDescriptor;
            physicsDescriptor.nonFinitePolicy = policy;
            physicsDescriptor.world.capacity = {maximumBodies, 32, 16, 4096};
            physicsDescriptor.budgets.maximumContactPairs = 32;
            physicsDescriptor.budgets.maximumContactConstraints = 16;
            physicsDescriptor.budgets.maximumInFlightPairs = 8;
            physicsDescriptor.budgets.scratchBytes = 1024 * 1024;
            auto physics = PhysicsWorldSettings::Capture(physicsDescriptor);
            auto character = Character::CharacterWorldSettings::Capture({});
            REQUIRE(physics.HasValue());
            REQUIRE(character.HasValue());
            return {physics.Value(), character.Value()};
        }

        [[nodiscard]] Runtime::FrameContext Context(const CancellationToken &cancellation) {
            return {1, {}, 0.0, 0, {}, false, cancellation};
        }

        /** @brief Creates the null runtime required by scene activation tests. */
        [[nodiscard]] std::unique_ptr<PhysicsRuntime> RequireRuntime() {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Null);
            REQUIRE(runtime.HasValue());
            return std::move(runtime).Value();
        }

        /** @brief Instantiates one runtime scene from a validated test definition. */
        [[nodiscard]] std::unique_ptr<Runtime::RuntimeScene> RequireScene(const Runtime::RuntimeSceneDefinition &definition) {
            auto scene = Runtime::RuntimeScene::Create(definition, Runtime::SceneRuntimeId{1});
            REQUIRE(scene.HasValue());
            return std::move(scene).Value();
        }

        class AssetSceneFixture final {
        public:
            AssetSceneFixture() : material(Asset(91)), materialType(AssetType()), loads(jobs, provider), service(registry, loads) {
                REQUIRE(registry.Publish({Record(material, materialType)}).status == AssetRegistryBuildStatus::Complete);
                provider.Insert(material, {1, 2, 3});
                REQUIRE(service.Startup(cancellation.Token()).HasValue());
            }

            ~AssetSceneFixture() {
                service.Shutdown();
                loads.Shutdown();
                jobs.Shutdown(ShutdownPolicy::Drain);
            }

            [[nodiscard]] Runtime::RuntimeSceneView Prepare(const Runtime::RuntimeSceneDefinition &definition) {
                REQUIRE(service.QueuePreparation(definition).HasValue());
                for (std::size_t iteration = 0; iteration < 2000; ++iteration) {
                    REQUIRE(
                        service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
                    if (const auto error = service.TakeOperationError(); error.has_value()) {
                        FAIL(error->message);
                        return {};
                    }
                    if (const auto active = service.ActiveScene();
                        active.has_value() && active->DefinitionRevision() == definition.Revision())
                        return *active;
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                FAIL("Timed out waiting for the asset-backed runtime scene.");
                return {};
            }

            Assets::AssetId material;
            Assets::AssetTypeId materialType;
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider provider;
            JobSystem jobs{JobSystemConfig{2, 16}};
            Assets::AssetLoadService loads;
            Runtime::RuntimeSceneService service;
            CancellationSource cancellation;
        };

    }  // namespace
}  // namespace Horo::Physics
