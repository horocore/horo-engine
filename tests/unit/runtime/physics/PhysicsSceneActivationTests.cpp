#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsSceneActivation.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "PhysicsSceneActivationTestSupport.h"
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
            REQUIRE_FALSE(candidate->FindBody({99}, {999}).has_value());
            REQUIRE_FALSE(candidate->FindShape({99}, {999}).has_value());
            REQUIRE_FALSE(candidate->FindConstraint({99}, {999}).has_value());
            candidate->Shutdown();
            Test::RequireError(candidate->ValidatePublication(), PhysicsErrors::InvalidState);
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

        TEST_CASE("Canonical Physics scene activation realizes analytic variants compound bodies and typed constraints",
                  "[physics][scene][activation][canonical][geometry]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            AssetSceneFixture assets;
            const auto definition = AnalyticVariantDefinition(assets.material, assets.materialType);
            const auto view = assets.Prepare(definition);
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant participant{*runtime.Value(), authority, Settings()};

            auto prepared = participant.Prepare(definition, view);
            if (prepared.HasError())
                FAIL(prepared.ErrorValue().message);
            REQUIRE(prepared.HasValue());
            auto *candidate = dynamic_cast<PhysicsSceneActivationCandidate *>(prepared.Value().get());
            REQUIRE(candidate != nullptr);
            REQUIRE(candidate->ValidatePublication().HasValue());
            REQUIRE(candidate->BodyBindings().size() == 3);
            REQUIRE(candidate->ShapeBindings().size() == 4);
            REQUIRE(candidate->ConstraintBindings().size() == 2);
            REQUIRE(candidate->FindBody({3}, {102}).has_value());
            REQUIRE(candidate->FindShape({2}, {202}).has_value());
            REQUIRE(candidate->FindShape({3}, {203}).has_value());
            REQUIRE(candidate->FindConstraint({1}, {500}).has_value());
            REQUIRE(candidate->FindConstraint({2}, {501}).has_value());
            candidate->Shutdown();
        }

        TEST_CASE("Physics scene activation rejects mixed sensor contributors before native admission",
                  "[physics][scene][activation][validation]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            AssetSceneFixture assets;
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{5}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = {1};
            entity.components.rigidBody = Runtime::RigidBodyComponent{.id = {10}, .body = {100}};
            entity.components.colliders =
                {Collider(20, 200, 1, 100, assets.material,
                          Runtime::PhysicsColliderSource{Runtime::PhysicsAnalyticCollider{Runtime::PhysicsBoxCollider{}}}),
                 Collider(21, 201, 1, 100, assets.material,
                          Runtime::PhysicsColliderSource{Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{}}}, true)};
            builder.Add(std::move(entity));
            REQUIRE(builder.RequireAsset({assets.material, assets.materialType}).HasValue());
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            const auto view = assets.Prepare(definition.Value());
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant participant{*runtime.Value(), authority, Settings()};

            const auto rejected = participant.Prepare(definition.Value(), view);
            Test::RequireError(rejected, PhysicsErrors::OperationUnsupported);
            REQUIRE(rejected.ErrorValue().message.find("mix sensor") != std::string::npos);
        }

        TEST_CASE("Physics scene activation rejects non-unit authored body scale with object context",
                  "[physics][scene][activation][validation]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            AssetSceneFixture assets;
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{6}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = {1};
            entity.localTransform.scale = {2.0F, 1.0F, 1.0F};
            entity.components.rigidBody = Runtime::RigidBodyComponent{.id = {10}, .body = {100}};
            entity.components.colliders = {
                Collider(20, 200, 1, 100, assets.material,
                         Runtime::PhysicsColliderSource{Runtime::PhysicsAnalyticCollider{Runtime::PhysicsBoxCollider{}}})};
            builder.Add(std::move(entity));
            REQUIRE(builder.RequireAsset({assets.material, assets.materialType}).HasValue());
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            const auto view = assets.Prepare(definition.Value());
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant participant{*runtime.Value(), authority, Settings()};

            const auto rejected = participant.Prepare(definition.Value(), view);
            Test::RequireError(rejected, PhysicsErrors::OperationUnsupported);
            REQUIRE(rejected.ErrorValue().message.find("unit world scale") != std::string::npos);
            REQUIRE(rejected.ErrorValue().message.find("object 1") != std::string::npos);
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

        TEST_CASE("Physics scene generation authority rejects foreign-thread advancement", "[physics][scene][activation][thread]") {
            PhysicsSceneActivationAuthority authority;
            bool collisionRejected = false;
            bool originRejected = false;
            std::thread foreign([&] {
                collisionRejected = authority.AdvanceCollisionFilterGeneration().ErrorValue().code.Value() ==
                                    PhysicsErrors::ThreadAffinityViolation.code.Value();
                originRejected =
                    authority.AdvanceOriginGeneration().ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
            });
            foreign.join();
            REQUIRE(collisionRejected);
            REQUIRE(originRejected);
            REQUIRE(authority.Capture() == PhysicsSceneActivationEvidence{1, 1});
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

        TEST_CASE("Play worlds commit and stop only at the lifecycle safe point", "[physics][play][lifecycle]") {
            auto runtime = RequireRuntime();
            const auto definition = Definition();
            auto scene = RequireScene(definition);
            PhysicsPlayWorldSession play{*runtime, Settings()};

            REQUIRE(play.Prepare(definition, scene->View()).HasValue());
            REQUIRE(play.HasPendingCandidate());
            REQUIRE_FALSE(play.IsActive());
            Test::RequireError(play.Commit(Runtime::RuntimePhase::FixedUpdate, scene->View()), PhysicsErrors::InvalidState);
            REQUIRE(play.HasPendingCandidate());
            REQUIRE(play.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, scene->View()).HasValue());
            const PhysicsWorldId first = play.WorldIdentity();
            REQUIRE(first.IsValid());
            REQUIRE_FALSE(play.HasPendingCandidate());

            REQUIRE(play.Prepare(definition, scene->View()).HasValue());
            Test::RequireError(play.Prepare(definition, scene->View()), PhysicsErrors::InvalidState);
            Test::RequireError(play.Stop(Runtime::RuntimePhase::FixedUpdate), PhysicsErrors::InvalidState);
            REQUIRE(play.IsActive());
            REQUIRE(play.Stop(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            REQUIRE(play.Stop(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            REQUIRE_FALSE(play.IsActive());
            REQUIRE_FALSE(play.HasPendingCandidate());
            REQUIRE_FALSE(play.WorldIdentity().IsValid());
            Test::RequireError(play.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, scene->View()),
                               PhysicsErrors::InvalidState);
        }

        TEST_CASE("Play preparation checks scene evidence and owner thread without disturbing an active world",
                  "[physics][play][validation]") {
            auto runtime = RequireRuntime();
            const auto definition = Definition();
            auto scene = RequireScene(definition);
            PhysicsPlayWorldSession play{*runtime, Settings()};
            REQUIRE(play.Prepare(definition, scene->View()).HasValue());
            REQUIRE(play.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, scene->View()).HasValue());
            const PhysicsWorldId first = play.WorldIdentity();

            Test::RequireError(play.Prepare(Definition(2), scene->View()), PhysicsErrors::QuerySnapshotStale);
            REQUIRE(play.WorldIdentity() == first);
            REQUIRE_FALSE(play.HasPendingCandidate());

            const Runtime::RuntimeSceneView preparedSource = scene->View();
            REQUIRE(play.Prepare(definition, preparedSource).HasValue());
            Runtime::SceneCommandBuffer commands;
            Math::Transform moved;
            moved.translation.x = 5.0F;
            commands.SetLocalTransform(*scene->View().Find({1}), moved);
            REQUIRE(scene->Commit(commands).HasValue());
            Test::RequireError(play.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, preparedSource),
                               PhysicsErrors::QuerySnapshotStale);
            REQUIRE(play.WorldIdentity() == first);
            REQUIRE_FALSE(play.HasPendingCandidate());

            bool rejected = false;
            std::thread foreign([&] {
                const auto result = play.Stop(Runtime::RuntimePhase::CommitDeferredLifecycleChanges);
                rejected = result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
            });
            foreign.join();
            REQUIRE(rejected);
            REQUIRE(play.WorldIdentity() == first);
            REQUIRE(play.Stop(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
        }

        TEST_CASE("Play candidate cannot publish after its process runtime shuts down", "[physics][play][shutdown]") {
            auto runtime = RequireRuntime();
            const auto definition = Definition();
            auto scene = RequireScene(definition);
            PhysicsPlayWorldSession play{*runtime, Settings()};
            REQUIRE(play.Prepare(definition, scene->View()).HasValue());

            runtime->Shutdown();
            Test::RequireError(play.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, scene->View()),
                               PhysicsErrors::InvalidState);
            REQUIRE_FALSE(play.IsActive());
            REQUIRE_FALSE(play.HasPendingCandidate());
        }

#if HORO_TEST_PHYSICS_NATIVE
        TEST_CASE("Play worlds isolate authored state and invalidate bindings on reload and stop", "[physics][play][canonical]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            AssetSceneFixture assets;
            const auto definition = PhysicsDefinition(assets.material, assets.materialType);
            const Runtime::RuntimeSceneView scene = assets.Prepare(definition);
            const Math::Transform authored = definition.Entities().front().localTransform;
            PhysicsPlayWorldSession first{*runtime.Value(), Settings()};
            PhysicsPlayWorldSession second{*runtime.Value(), Settings()};

            REQUIRE(first.Prepare(definition, scene).HasValue());
            REQUIRE(second.Prepare(definition, scene).HasValue());
            REQUIRE(first.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, scene).HasValue());
            REQUIRE(second.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, scene).HasValue());
            REQUIRE(first.WorldIdentity() != second.WorldIdentity());
            const BodyHandle firstBody = first.ResolveBody({1}, {100}).Value();
            const BodyHandle secondBody = second.ResolveBody({1}, {100}).Value();
            REQUIRE(first.ValidateBody(firstBody).HasValue());
            REQUIRE(second.ValidateBody(secondBody).HasValue());
            Test::RequireError(second.ValidateBody(firstBody), PhysicsErrors::HandleWorldMismatch);
            Test::RequireError(first.ResolveBody({99}, {100}), PhysicsErrors::HandleStale);

            const auto tick = PhysicsFixedTickInput{.simulationTick = 1,
                                                    .sceneGeneration = scene.RuntimeId().value,
                                                    .fixedDelta = Duration::FromNanoseconds(16'666'667)};
            Test::RequireError(first.AdvanceFixedTick(
                                   {.simulationTick = 1, .sceneGeneration = 999, .fixedDelta = Duration::FromNanoseconds(16'666'667)}),
                               PhysicsErrors::QuerySnapshotStale);
            REQUIRE(first.AdvanceFixedTick(tick).HasValue());
            REQUIRE(first.PublishedTick().Value().completedTick == 1);
            REQUIRE(second.PublishedTick().Value().completedTick == 0);
            REQUIRE(definition.Entities().front().localTransform == authored);
            REQUIRE(scene.Get(*scene.Find({1})).Value().localTransform->translation == authored.translation);

            REQUIRE(first.Prepare(definition, scene).HasValue());
            const PhysicsWorldId oldWorld = first.WorldIdentity();
            REQUIRE(first.ValidateBody(firstBody).HasValue());
            REQUIRE(first.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, scene).HasValue());
            REQUIRE(first.WorldIdentity() != oldWorld);
            REQUIRE(first.PublishedTick().Value().completedTick == 0);
            Test::RequireError(first.ValidateBody(firstBody), PhysicsErrors::HandleWorldMismatch);
            REQUIRE(first.ResolveBody({1}, {100}).HasValue());
            REQUIRE(second.ValidateBody(secondBody).HasValue());
            REQUIRE(first.Stop(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            Test::RequireError(first.ValidateBody(firstBody), PhysicsErrors::InvalidState);
            REQUIRE(second.ValidateBody(secondBody).HasValue());
        }

        TEST_CASE("Failed play-world replacement preserves the old world and its bindings", "[physics][play][rollback]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            AssetSceneFixture assets;
            const auto definition = PhysicsDefinition(assets.material, assets.materialType, 1, false);
            const Runtime::RuntimeSceneView scene = assets.Prepare(definition);
            PhysicsPlayWorldSession play{*runtime.Value(), Settings(1)};
            REQUIRE(play.Prepare(definition, scene).HasValue());
            REQUIRE(play.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, scene).HasValue());
            const BodyHandle oldBody = play.ResolveBody({1}, {100}).Value();
            const PhysicsWorldId oldWorld = play.WorldIdentity();

            const auto oversized = PhysicsDefinition(assets.material, assets.materialType, 2, false, 2);
            const Runtime::RuntimeSceneView replacement = assets.Prepare(oversized);
            Test::RequireError(play.Prepare(oversized, replacement), PhysicsErrors::CapacityExceeded);
            REQUIRE(play.WorldIdentity() == oldWorld);
            REQUIRE(play.ValidateBody(oldBody).HasValue());
            REQUIRE_FALSE(play.HasPendingCandidate());
        }
#endif
    }  // namespace
}  // namespace Horo::Physics
