#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsSceneActivation.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace {
        using namespace Assets;

        [[nodiscard]] Runtime::RuntimeSceneDefinition Definition(const std::uint64_t revision = 1) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{revision}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{1};
            builder.Add(std::move(entity));
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
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
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
        }

        [[nodiscard]] PhysicsSceneActivationSettings Settings(const std::uint32_t maximumBodies = 16) {
            PhysicsWorldSettingsDescriptor physicsDescriptor;
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

        TEST_CASE("Physics scene participant recreation preserves process-runtime world identity monotonicity",
                  "[physics][scene][activation][identity]") {
            auto runtime = RequireRuntime();
            PhysicsSceneActivationAuthority authority;
            const auto definition = Definition();
            auto scene = RequireScene(definition);

            PhysicsSceneActivationParticipant first{*runtime, authority, Settings()};
            auto firstCandidate = first.Prepare(definition, scene->View());
            REQUIRE(firstCandidate.HasValue());
            PhysicsSceneActivationParticipant replacement{*runtime, authority, Settings()};
            auto replacementCandidate = replacement.Prepare(definition, scene->View());
            REQUIRE(replacementCandidate.HasValue());
            REQUIRE(firstCandidate.Value()->ValidatePublication().HasValue());
            REQUIRE(replacementCandidate.Value()->ValidatePublication().HasValue());

            replacementCandidate.Value()->Shutdown();
            firstCandidate.Value()->Shutdown();
        }

        TEST_CASE("Physics scene activation rejects required content when the selected runtime omits capabilities",
                  "[physics][scene][activation][capability]") {
            auto runtime = RequireRuntime();
            PhysicsSceneActivationAuthority authority;
            const auto material = Asset(92);
            const auto definition = PhysicsDefinition(material, AssetType(), 1, false, 1, false);
            auto scene = RequireScene(definition);
            PhysicsSceneActivationParticipant participant{*runtime, authority, Settings()};

            const auto candidate = participant.Prepare(definition, scene->View());
            Test::RequireError(candidate, PhysicsErrors::CapabilityUnavailable);
        }

#if HORO_TEST_PHYSICS_NATIVE
        TEST_CASE("Canonical Physics scene activation publishes complete authored bindings", "[physics][scene][activation][canonical]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            AssetSceneFixture assets;
            const auto definition = PhysicsDefinition(assets.material, assets.materialType);
            const auto view = assets.Prepare(definition);
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant participant{*runtime.Value(), authority, Settings()};

            auto prepared = participant.Prepare(definition, view);
            REQUIRE(prepared.HasValue());
            auto *candidate = dynamic_cast<PhysicsSceneActivationCandidate *>(prepared.Value().get());
            REQUIRE(candidate != nullptr);
            REQUIRE(candidate->ValidatePublication().HasValue());
            REQUIRE(candidate->BodyBindings().size() == 2);
            REQUIRE(candidate->ShapeBindings().size() == 2);
            REQUIRE(candidate->ConstraintBindings().size() == 1);

            const auto firstBody = candidate->FindBody({1}, {100});
            const auto secondBody = candidate->FindBody({2}, {101});
            const auto firstShape = candidate->FindShape({1}, {200});
            const auto secondShape = candidate->FindShape({2}, {201});
            const auto constraint = candidate->FindConstraint({1}, {500});
            REQUIRE(firstBody.has_value());
            REQUIRE(secondBody.has_value());
            REQUIRE(firstShape.has_value());
            REQUIRE(secondShape.has_value());
            REQUIRE(constraint.has_value());
            REQUIRE(firstBody->world == candidate->WorldIdentity());
            REQUIRE(secondBody->world == candidate->WorldIdentity());
            REQUIRE(firstShape->world == candidate->WorldIdentity());
            REQUIRE(secondShape->world == candidate->WorldIdentity());
            REQUIRE(constraint->world == candidate->WorldIdentity());
            candidate->Shutdown();
        }

        TEST_CASE("Canonical Physics scene activation rolls back staged capacity failures and accepts a corrected retry",
                  "[physics][scene][activation][rollback]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            AssetSceneFixture assets;
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant participant{*runtime.Value(), authority, Settings(1)};

            const auto oversized = PhysicsDefinition(assets.material, assets.materialType, 2, false, 1);
            const auto oversizedView = assets.Prepare(oversized);
            const auto rejected = participant.Prepare(oversized, oversizedView);
            Test::RequireError(rejected, PhysicsErrors::CapacityExceeded);
            REQUIRE(rejected.ErrorValue().message.find("object 2") != std::string::npos);

            const auto corrected = PhysicsDefinition(assets.material, assets.materialType, 1, false, 2);
            const auto correctedView = assets.Prepare(corrected);
            auto accepted = participant.Prepare(corrected, correctedView);
            REQUIRE(accepted.HasValue());
            auto *candidate = dynamic_cast<PhysicsSceneActivationCandidate *>(accepted.Value().get());
            REQUIRE(candidate != nullptr);
            REQUIRE(candidate->BodyBindings().size() == 1);
            REQUIRE(candidate->ShapeBindings().size() == 1);
            REQUIRE(candidate->ConstraintBindings().empty());
            REQUIRE(candidate->FindBody({1}, {100}).has_value());
            REQUIRE(candidate->FindShape({1}, {200}).has_value());
            candidate->Shutdown();
        }

        TEST_CASE("Physics scene asset failures preserve authored context before native preparation",
                  "[physics][scene][activation][diagnostics]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            PhysicsSceneActivationAuthority authority;
            const auto material = Asset(93);
            const auto definition = PhysicsDefinition(material, AssetType(), 1, false, 1, false);
            auto scene = RequireScene(definition);
            PhysicsSceneActivationParticipant participant{*runtime.Value(), authority, Settings()};

            const auto rejected = participant.Prepare(definition, scene->View());
            Test::RequireError(rejected, PhysicsErrors::MaterialDescriptorInvalid);
            REQUIRE(rejected.ErrorValue().message.find("object 1") != std::string::npos);
            REQUIRE(rejected.ErrorValue().message.find(material.ToString()) != std::string::npos);
        }
#endif

        TEST_CASE("Physics scene candidate rejects authoritative generation changes before publication",
                  "[physics][scene][activation][generation]") {
            auto runtime = RequireRuntime();
            PhysicsSceneActivationAuthority authority;
            const auto definition = Definition();
            auto scene = RequireScene(definition);
            PhysicsSceneActivationParticipant participant{*runtime, authority, Settings()};
            auto candidate = participant.Prepare(definition, scene->View());
            REQUIRE(candidate.HasValue());

            REQUIRE(authority.AdvanceOriginGeneration().HasValue());
            Test::RequireError(candidate.Value()->ValidatePublication(), PhysicsErrors::QuerySnapshotStale);
            candidate.Value()->Shutdown();
        }

        TEST_CASE("Runtime scene replaces and tears down real Physics aggregate candidates", "[physics][scene][activation][replacement]") {
            auto runtime = RequireRuntime();
            PhysicsSceneActivationAuthority authority;
            Runtime::RuntimeSceneService scenes;
            auto participant = std::make_unique<PhysicsSceneActivationParticipant>(*runtime, authority, Settings());
            REQUIRE(scenes.AddActivationParticipant(std::move(participant)).HasValue());
            CancellationSource cancellation;
            REQUIRE(scenes.Startup(cancellation.Token()).HasValue());

            REQUIRE(scenes.QueuePreparation(Definition()).HasValue());
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE(scenes.ActiveScene().has_value());
            const Runtime::SceneRuntimeId first = scenes.ActiveScene()->RuntimeId();

            REQUIRE(scenes.QueuePreparation(Definition(2)).HasValue());
            REQUIRE(authority.AdvanceCollisionFilterGeneration().HasValue());
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE(scenes.ActiveScene()->RuntimeId() == first);
            const auto stale = scenes.TakeOperationError();
            REQUIRE(stale.has_value());
            REQUIRE(stale->code.Value() == PhysicsErrors::QuerySnapshotStale.code.Value());

            REQUIRE(scenes.QueuePreparation(Definition(3)).HasValue());
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE(scenes.ActiveScene()->RuntimeId() != first);
            REQUIRE_FALSE(scenes.TakeOperationError().has_value());

            REQUIRE(scenes.QueueUnload().HasValue());
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE_FALSE(scenes.ActiveScene().has_value());
        }
    }  // namespace
}  // namespace Horo::Physics
