#include "Horo/Physics/PhysicsSceneActivation.h"
#include "PhysicsTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>
#include <variant>

namespace Horo::Physics {
    namespace {
        [[nodiscard]] Runtime::RuntimeSceneDefinition RequireDefinition(std::uint64_t revision, Runtime::RuntimeEntityDefinition entity) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{revision}};
            builder.Add(std::move(entity));
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
        }

        [[nodiscard]] Runtime::RuntimeSceneDefinition Definition(const std::uint64_t revision = 1) {
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{1};
            return RequireDefinition(revision, std::move(entity));
        }

        [[nodiscard]] Runtime::RuntimeSceneDefinition TriggerDefinition(
            const std::uint64_t revision = 1, const Runtime::ColliderShapeType shape = Runtime::ColliderShapeType::Box,
            const bool enabled = true, const bool uniformScale = false) {
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{1};
            entity.localTransform.translation = {2.0F, 3.0F, 4.0F};
            entity.localTransform.scale = uniformScale ? Math::Vec3{2.0F, 2.0F, 2.0F} : Math::Vec3{2.0F, 1.0F, 3.0F};
            entity.components.triggerVolume = Runtime::TriggerVolumeComponent{.shape = shape, .enabled = enabled};
            return RequireDefinition(revision, std::move(entity));
        }

        [[nodiscard]] PhysicsSceneActivationSettings Settings() {
            auto physics = PhysicsWorldSettings::Capture({});
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

        void RequireActivated(Runtime::RuntimeSceneService &scenes, const Runtime::RuntimeSceneDefinition &definition,
                              const CancellationToken &cancellation) {
            REQUIRE(scenes.QueuePreparation(definition).HasValue());
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation)).HasValue());
            REQUIRE(scenes.ActiveScene().has_value());
        }

        void RequireStartedSceneService(Runtime::RuntimeSceneService &scenes, PhysicsRuntime &runtime,
                                        PhysicsSceneActivationAuthority &authority, CancellationSource &cancellation) {
            auto participant = std::make_unique<PhysicsSceneActivationParticipant>(runtime, authority, Settings());
            REQUIRE(scenes.AddActivationParticipant(std::move(participant)).HasValue());
            REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
        }

        void RequireUnloaded(Runtime::RuntimeSceneService &scenes, const CancellationToken &cancellation) {
            REQUIRE(scenes.QueueUnload().HasValue());
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation)).HasValue());
            REQUIRE_FALSE(scenes.ActiveScene().has_value());
        }

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

        TEST_CASE("Legacy trigger volumes convert to deterministic world-space sensor fixtures", "[physics][scene][trigger][conversion]") {
            const auto definition = TriggerDefinition();
            const auto bindings = BuildPhysicsTriggerVolumeBindings(definition);
            REQUIRE(bindings.HasValue());
            REQUIRE(bindings.Value().size() == 1);
            REQUIRE(bindings.Value().front().object == Runtime::SceneObjectId{1});
            REQUIRE(bindings.Value().front().fixture.trigger);
            REQUIRE(bindings.Value().front().fixture.response == PhysicsQueryFixtureResponse::Overlap);
            const Math::Vec3 expectedTranslation{2.0F, 3.0F, 4.0F};
            REQUIRE(bindings.Value().front().fixture.pose.translation == expectedTranslation);
            REQUIRE(bindings.Value().front().fixture.pose.rotation == Math::Quaternion::Identity());
            REQUIRE(bindings.Value().front().fixture.layer.IsValid());
            REQUIRE(bindings.Value().front().fixture.profile.IsValid());
            REQUIRE(bindings.Value().front().fixture.channel.IsValid());

            const auto &box = std::get<PhysicsBoxShape>(bindings.Value().front().fixture.shape);
            const Math::Vec3 expectedHalfExtents{1.0F, 0.5F, 1.5F};
            REQUIRE(box.halfExtentsMeters == expectedHalfExtents);
        }

        TEST_CASE("Trigger conversion resolves shared parent transforms and analytic shape variants",
                  "[physics][scene][trigger][conversion]") {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{5}};

            Runtime::RuntimeEntityDefinition parent;
            parent.object = Runtime::SceneObjectId{10};
            parent.localTransform.translation = {10.0F, 20.0F, 30.0F};
            builder.Add(std::move(parent));

            Runtime::RuntimeEntityDefinition capsule;
            capsule.object = Runtime::SceneObjectId{2};
            capsule.parent = Runtime::SceneObjectId{10};
            capsule.localTransform.translation = {1.0F, 2.0F, 3.0F};
            capsule.components.triggerVolume = Runtime::TriggerVolumeComponent{.shape = Runtime::ColliderShapeType::Capsule};
            builder.Add(std::move(capsule));

            Runtime::RuntimeEntityDefinition plane;
            plane.object = Runtime::SceneObjectId{3};
            plane.parent = Runtime::SceneObjectId{10};
            plane.localTransform.translation = {4.0F, 5.0F, 6.0F};
            plane.components.triggerVolume = Runtime::TriggerVolumeComponent{.shape = Runtime::ColliderShapeType::StaticPlane};
            builder.Add(std::move(plane));

            const auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            const auto bindings = BuildPhysicsTriggerVolumeBindings(definition.Value());
            REQUIRE(bindings.HasValue());
            REQUIRE(bindings.Value().size() == 2);
            REQUIRE(bindings.Value()[0].object == Runtime::SceneObjectId{2});
            REQUIRE(bindings.Value()[0].fixture.pose.translation == Math::Vec3{11.0F, 22.0F, 33.0F});
            REQUIRE(std::holds_alternative<PhysicsCapsuleShape>(bindings.Value()[0].fixture.shape));
            REQUIRE(bindings.Value()[1].object == Runtime::SceneObjectId{3});
            REQUIRE(bindings.Value()[1].fixture.pose.translation == Math::Vec3{14.0F, 25.0F, 36.0F});
            REQUIRE(std::holds_alternative<PhysicsStaticPlaneShape>(bindings.Value()[1].fixture.shape));
        }

        TEST_CASE("Trigger conversion rejects unsupported geometry and degenerate scale", "[physics][scene][trigger][validation]") {
            const auto unsupported = BuildPhysicsTriggerVolumeBindings(TriggerDefinition(1, static_cast<Runtime::ColliderShapeType>(255)));
            Test::RequireError(unsupported, PhysicsErrors::OperationUnsupported);

            const auto nonUniformSphere = BuildPhysicsTriggerVolumeBindings(TriggerDefinition(1, Runtime::ColliderShapeType::Sphere));
            Test::RequireError(nonUniformSphere, PhysicsErrors::OperationUnsupported);

            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{2}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{1};
            entity.localTransform.scale = {0.0F, 1.0F, 1.0F};
            entity.components.triggerVolume = Runtime::TriggerVolumeComponent{};
            builder.Add(std::move(entity));
            auto zeroScale = std::move(builder).Build();
            REQUIRE(zeroScale.HasValue());
            Test::RequireError(BuildPhysicsTriggerVolumeBindings(zeroScale.Value()), PhysicsErrors::DescriptorInvalid);
        }

        TEST_CASE("Trigger conversion preserves hierarchy validation and rejects shear", "[physics][scene][trigger][validation]") {
            Runtime::SceneDefinitionBuilder cycleBuilder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{3}};
            Runtime::RuntimeEntityDefinition first;
            first.object = Runtime::SceneObjectId{1};
            first.parent = Runtime::SceneObjectId{2};
            Runtime::RuntimeEntityDefinition second;
            second.object = Runtime::SceneObjectId{2};
            second.parent = Runtime::SceneObjectId{1};
            cycleBuilder.Add(std::move(first));
            cycleBuilder.Add(std::move(second));
            REQUIRE(std::move(cycleBuilder).Build().HasError());

            Runtime::SceneDefinitionBuilder shearBuilder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{4}};
            Runtime::RuntimeEntityDefinition parent;
            parent.object = Runtime::SceneObjectId{10};
            parent.localTransform.scale = {2.0F, 1.0F, 1.0F};
            Runtime::RuntimeEntityDefinition trigger;
            trigger.object = Runtime::SceneObjectId{1};
            trigger.parent = Runtime::SceneObjectId{10};
            trigger.localTransform.rotation = {0.0F, 0.0F, 0.38268343F, 0.9238795F};
            trigger.components.triggerVolume = Runtime::TriggerVolumeComponent{};
            shearBuilder.Add(std::move(parent));
            shearBuilder.Add(std::move(trigger));
            const auto shearDefinition = std::move(shearBuilder).Build();
            REQUIRE(shearDefinition.HasValue());
            Test::RequireError(BuildPhysicsTriggerVolumeBindings(shearDefinition.Value()), PhysicsErrors::OperationUnsupported);
        }

        TEST_CASE("Disabled trigger volumes do not consume Physics fixture admission", "[physics][scene][trigger][lifecycle]") {
            const auto definition = TriggerDefinition(1, static_cast<Runtime::ColliderShapeType>(255), false);
            const auto bindings = BuildPhysicsTriggerVolumeBindings(definition);
            REQUIRE(bindings.HasValue());
            REQUIRE(bindings.Value().empty());

            auto runtime = RequireRuntime();
            PhysicsSceneActivationAuthority authority;
            auto scene = RequireScene(definition);
            PhysicsSceneActivationParticipant participant{*runtime, authority, Settings()};
            auto candidate = participant.Prepare(definition, scene->View());
            REQUIRE(candidate.HasValue());
            candidate.Value()->Shutdown();
        }

        TEST_CASE("Null Physics rejects enabled trigger activation without publishing a candidate",
                  "[physics][scene][trigger][capability]") {
            auto runtime = RequireRuntime();
            PhysicsSceneActivationAuthority authority;
            const auto definition = TriggerDefinition();
            auto scene = RequireScene(definition);
            PhysicsSceneActivationParticipant participant{*runtime, authority, Settings()};
            const auto candidate = participant.Prepare(definition, scene->View());
            Test::RequireError(candidate, PhysicsErrors::CapabilityUnavailable);
        }

        TEST_CASE("Physics scene participant rejects an invalid runtime scene view before activation",
                  "[physics][scene][activation][validation]") {
            auto runtime = RequireRuntime();
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant participant{*runtime, authority, Settings()};
            Test::RequireError(participant.Prepare(Definition(), Runtime::RuntimeSceneView{}), PhysicsErrors::WorldInvalid);
        }

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
            CancellationSource cancellation;
            RequireStartedSceneService(scenes, *runtime, authority, cancellation);

            RequireActivated(scenes, Definition(), cancellation.Token());
            const Runtime::SceneRuntimeId first = scenes.ActiveScene()->RuntimeId();

            REQUIRE(scenes.QueuePreparation(Definition(2)).HasValue());
            REQUIRE(authority.AdvanceCollisionFilterGeneration().HasValue());
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE(scenes.ActiveScene()->RuntimeId() == first);
            const auto stale = scenes.TakeOperationError();
            REQUIRE(stale.has_value());
            REQUIRE(stale->code.Value() == PhysicsErrors::QuerySnapshotStale.code.Value());

            RequireActivated(scenes, Definition(3), cancellation.Token());
            REQUIRE(scenes.ActiveScene()->RuntimeId() != first);
            REQUIRE_FALSE(scenes.TakeOperationError().has_value());

            RequireUnloaded(scenes, cancellation.Token());
        }

#if HORO_TEST_PHYSICS_NATIVE
        TEST_CASE("Native trigger fixtures reconcile across rebuild and unload", "[physics][scene][trigger][native][lifecycle]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            PhysicsSceneActivationAuthority authority;
            Runtime::RuntimeSceneService scenes;
            CancellationSource cancellation;
            RequireStartedSceneService(scenes, *runtime.Value(), authority, cancellation);

            RequireActivated(scenes, TriggerDefinition(1, Runtime::ColliderShapeType::Box), cancellation.Token());

            RequireActivated(scenes, TriggerDefinition(2, Runtime::ColliderShapeType::Sphere, true, true), cancellation.Token());
            REQUIRE_FALSE(scenes.TakeOperationError().has_value());

            RequireUnloaded(scenes, cancellation.Token());
        }
#endif
    }  // namespace
}  // namespace Horo::Physics
