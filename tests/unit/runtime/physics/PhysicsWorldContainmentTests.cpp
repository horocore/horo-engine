#include "Horo/Physics/PhysicsBodyDynamics.h"
#include "Horo/Physics/PhysicsTransformAuthority.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"
#include "PhysicsWorldInternal.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <limits>

namespace Horo::Physics {
    TEST_CASE("Non-finite values reject at every typed physical-input boundary", "[physics][nonfinite][admission]") {
        const float value = GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                     -std::numeric_limits<float>::infinity());
        const PhysicsWorldId owner = PhysicsWorldId::Create(935).Value();
        const BodyHandle body{owner, {0, 1}};
        const Math::Vec3 corrupt{value, 0, 0};
        PhysicsBodyDynamicsCommand command{.simulationTick = 1,
                                           .sceneGeneration = 7,
                                           .body = body,
                                           .source = PhysicsCommandSourceId::Create(1).Value(),
                                           .sourceSequence = 1};
        for (const PhysicsBodyDynamicsPayload &payload :
             {PhysicsBodyDynamicsPayload{PhysicsLinearForce{corrupt}}, PhysicsBodyDynamicsPayload{PhysicsLinearImpulse{corrupt}},
              PhysicsBodyDynamicsPayload{PhysicsTorque{corrupt}}, PhysicsBodyDynamicsPayload{PhysicsAngularImpulse{corrupt}},
              PhysicsBodyDynamicsPayload{PhysicsLinearVelocityControl{corrupt}},
              PhysicsBodyDynamicsPayload{PhysicsAngularVelocityControl{corrupt}},
              PhysicsBodyDynamicsPayload{PhysicsLinearForce{{}, corrupt}}, PhysicsBodyDynamicsPayload{PhysicsLinearImpulse{{}, corrupt}}}) {
            command.payload = payload;
            Test::RequireError(ValidatePhysicsBodyDynamicsCommand(command, owner, 7, 1, PhysicsMotionType::Dynamic, {}),
                               PhysicsErrors::DescriptorInvalid);
        }
        PhysicsKinematicTargetCommand target{
            .identity = {.simulationTick = 1, .sceneGeneration = 7, .body = body, .source = command.source, .sourceSequence = 1}};
        target.targetPose.translation = corrupt;
        Test::RequireError(ValidatePhysicsKinematicTargetCommand(target, owner, 7, 1), PhysicsErrors::DescriptorInvalid);
        target.targetPose.translation = {};
        target.targetPose.rotation.w = value;
        Test::RequireError(ValidatePhysicsKinematicTargetCommand(target, owner, 7, 1), PhysicsErrors::DescriptorInvalid);
    }

#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        /** @brief Queues the next-tick mutation that quarantine must discard. */
        [[nodiscard]] PhysicsStructuralCommand FutureMutation(const BodyHandle body) {
            const auto target = static_cast<std::uint64_t>(body.slot.index) + 1;
            return {.order = {.simulationTick = 2,
                              .worldGeneration = body.world.Value(),
                              .sceneGeneration = 7,
                              .targetKind = PhysicsCommandTargetKind::Body,
                              .targetIdentity = target,
                              .commandKind = PhysicsStructuralCommandKind::Change,
                              .source = PhysicsCommandSourceId::Create(1).Value(),
                              .sourceSequence = 1},
                    .bodyMutation = PhysicsBodyMutation{.body = body, .wake = PhysicsBodyWakePolicy::Wake}};
        }

        /** @brief Creates two independently addressable finite kinematic bodies. */
        std::array<BodyHandle, 2> KinematicPair(PhysicsWorld &world) {
            PhysicsBodyDescriptor body;
            body.shape = world.CreateSceneShape(PhysicsBoxShape{}).Value();
            body.motion = PhysicsMotionType::Kinematic;
            const auto first = world.CreateSceneBody({body, false}).Value();
            body.pose.translation.x = 4;
            return {first, world.CreateSceneBody({body, false}).Value()};
        }

        /** @brief Checks that containment preserves exact world, body, scene and tick evidence. */
        void RequireNonFiniteDiagnostic(const PhysicsWorld &world, const PhysicsWorldId identity, const BodyHandle corrupt) {
            REQUIRE(world.LastDiagnostic().has_value());
            const auto &diagnostic = *world.LastDiagnostic();
            REQUIRE(diagnostic.code.Value() == PhysicsErrors::BodyStateNonFinite.code.Value());
            REQUIRE(std::get<PhysicsWorldId>(diagnostic.context[0].value) == identity);
            REQUIRE(std::get<BodyHandle>(diagnostic.context[1].value) == corrupt);
            REQUIRE(std::get<std::uint64_t>(diagnostic.context[2].value) == 7);
            REQUIRE(std::get<std::uint64_t>(diagnostic.context[3].value) == 1);
            REQUIRE(std::get<std::uint64_t>(diagnostic.context[4].value) == 777);
        }

        /** @brief Checks the survivor and stale-handle behavior after body quarantine. */
        void RequireQuarantineOutcome(PhysicsWorld &world, const BodyHandle corrupt, const BodyHandle healthy) {
            REQUIRE(world.State() == PhysicsWorldState::ActiveSolver);
            REQUIRE(world.PublishedTick().publicationRevision == 1);
            Test::RequireError(world.ReadSceneBodyPolicy(corrupt), PhysicsErrors::HandleStale);
            REQUIRE(world.TickStatistics().pendingCommands == 0);
            REQUIRE(world.ReadSceneBodyReconciliation(healthy).HasValue());
            REQUIRE(world.AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                        .HasValue());
        }

        /** @brief Checks that terminal teardown does not overwrite the original bounded cause. */
        void RequireRetainedDiagnostic(const PhysicsWorld &world, const PhysicsDiagnosticRecord &failure) {
            REQUIRE(world.LastDiagnostic()->contextCount == failure.contextCount);
            for (std::size_t index = 0; index < failure.contextCount; ++index)
                REQUIRE(world.LastDiagnostic()->context[index].value == failure.context[index].value);
            REQUIRE(world.LastFailure()->code.Value() == failure.code.Value());
        }

        /** @brief Verifies terminal publication, teardown retention and explicit reset recovery. */
        void RequireTerminalRecovery(PhysicsWorld &world, const PhysicsFixedTickInput &tick, const PhysicsPublishedTick &prior,
                                     const bool resetBeforeTeardown) {
            REQUIRE(world.PublishedTick().completedTick == prior.completedTick);
            REQUIRE(world.PublishedTick().publicationRevision == prior.publicationRevision);
            REQUIRE(world.PublishedTick().transformTick == prior.transformTick);
            REQUIRE(world.PublishedTick().queryTick == prior.queryTick);
            REQUIRE(world.PublishedTick().eventTick == prior.eventTick);
            const auto failure = world.LastDiagnostic().value();
            Test::RequireError(world.AdvanceFixedTick(tick), PhysicsErrors::InvalidState);
            RequireRetainedDiagnostic(world, failure);
            if (resetBeforeTeardown) {
                REQUIRE(world.Reset().HasValue());
                REQUIRE_FALSE(world.LastFailure().has_value());
                REQUIRE_FALSE(world.LastDiagnostic().has_value());
                REQUIRE(world.Activate(PhysicsWorldId::Create(937).Value()).HasValue());
                return;
            }
            REQUIRE(world.UnloadScene().HasValue());
            world.Shutdown();
            RequireRetainedDiagnostic(world, failure);
            Test::RequireError(world.Reset(), PhysicsErrors::InvalidState);
        }

        /** @brief Exercises all descriptor pose and velocity components before native body admission. */
        void RejectDescriptorComponents(PhysicsWorld &world, PhysicsBodyDescriptor &descriptor, const std::span<const float> values) {
            const std::array components{&descriptor.pose.translation.x, &descriptor.pose.translation.y, &descriptor.pose.translation.z,
                                        &descriptor.pose.rotation.x,    &descriptor.pose.rotation.y,    &descriptor.pose.rotation.z,
                                        &descriptor.pose.rotation.w,    &descriptor.linearVelocity.x,   &descriptor.linearVelocity.y,
                                        &descriptor.linearVelocity.z,   &descriptor.angularVelocity.x,  &descriptor.angularVelocity.y,
                                        &descriptor.angularVelocity.z};
            for (float *component : components) {
                const float original = *component;
                for (const float value : values) {
                    *component = value;
                    Test::RequireError(world.CreateSceneBody({.body = descriptor, .sensor = false, .sceneEntity = 701}),
                                       PhysicsErrors::DescriptorInvalid);
                }
                *component = original;
            }
        }

        /** @brief Checks terminal failure and diagnostic retention after shutdown. */
        void RequireFailedWorldOutcome(PhysicsWorld &world, const Result<void> &stepped, const PhysicsFixedTickInput &tick) {
            Test::RequireError(stepped, PhysicsErrors::BodyStateNonFinite);
            REQUIRE(world.State() == PhysicsWorldState::Failed);
            REQUIRE(world.PublishedTick().publicationRevision == 0);
            Test::RequireError(world.AdvanceFixedTick(tick), PhysicsErrors::InvalidState);
            world.Shutdown();
            REQUIRE(world.LifecycleCause() == PhysicsWorldLifecycleCause::FatalSolverError);
            REQUIRE(world.LastFailure()->code.Value() == PhysicsErrors::BodyStateNonFinite.code.Value());
            REQUIRE(world.LastDiagnostic()->code.Value() == PhysicsErrors::BodyStateNonFinite.code.Value());
        }
    }  // namespace

    TEST_CASE("Non-finite body descriptors and mutations reject before native admission", "[physics][native][nonfinite]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(929).Value()).HasValue());
        const ShapeHandle shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        PhysicsBodyDescriptor descriptor;
        descriptor.shape = shape;
        descriptor.mass = PhysicsNoMass{};
        const float values[]{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()};
        RejectDescriptorComponents(*world, descriptor, values);
        REQUIRE(world->LastDiagnostic().has_value());
        REQUIRE(std::get<PhysicsWorldId>(world->LastDiagnostic()->context[0].value) == world->Identity());
        REQUIRE(std::get<std::uint64_t>(world->LastDiagnostic()->context[1].value) == 701);
        const BodyHandle body = world->CreateSceneBody({.body = descriptor, .sensor = false, .sceneEntity = 702}).Value();
        auto command = FutureMutation(body);
        command.order.simulationTick = 1;
        for (const float value : values) {
            command.bodyMutation->linearVelocity = Math::Vec3{value, 0.0F, 0.0F};
            Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::DescriptorInvalid);
            command.bodyMutation->linearVelocity.reset();
            command.bodyMutation->angularVelocity = Math::Vec3{0.0F, value, 0.0F};
            Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::DescriptorInvalid);
            command.bodyMutation->angularVelocity.reset();
        }
        REQUIRE(world->LastDiagnostic()->code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        REQUIRE(std::get<BodyHandle>(world->LastDiagnostic()->context[1].value) == body);
        REQUIRE(std::get<std::uint64_t>(world->LastDiagnostic()->context[4].value) == 702);
        REQUIRE(world->State() == PhysicsWorldState::ActiveSolver);
        REQUIRE(world->PublishedTick().publicationRevision == 0);
    }

    TEST_CASE("Quarantine preserves healthy command source sequences in current and deferred frames", "[physics][native][nonfinite]") {
        const bool postStep = GENERATE(false, true);
        const bool deferred = GENERATE(false, true);
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto settings = Test::SmallWorldSettings().Values();
        settings.nonFinitePolicy = PhysicsNonFinitePolicy::QuarantineBody;
        auto world = runtime->PrepareWorld(PhysicsWorldSettings::Capture(settings).Value()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(939).Value()).HasValue());
        const auto bodies = KinematicPair(*world);
        auto corruptCommand = FutureMutation(bodies[0]);
        corruptCommand.order.simulationTick = deferred ? 2 : 1;
        auto healthyCommand = FutureMutation(bodies[1]);
        healthyCommand.order.simulationTick = corruptCommand.order.simulationTick;
        healthyCommand.order.sourceSequence = 2;
        healthyCommand.bodyMutation->linearVelocity = Math::Vec3{1, 0, 0};
        REQUIRE(world->QueueStructuralCommand(corruptCommand).HasValue());
        REQUIRE(world->QueueStructuralCommand(healthyCommand).HasValue());
        REQUIRE(PhysicsWorldContainmentTestAccess::Inject(*world, bodies[0], std::numeric_limits<float>::quiet_NaN(), 0, postStep));
        PhysicsFixedTickInput tick{.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)};
        REQUIRE(world->AdvanceFixedTick(tick).HasValue());
        const std::uint32_t pending = deferred ? 1 : 0;
        REQUIRE(world->TickStatistics().pendingCommands == pending);
        tick.simulationTick = 2;
        REQUIRE(world->AdvanceFixedTick(tick).HasValue());
        REQUIRE(world->PublishedTick().appliedCommands == pending);
        REQUIRE(world->ReadSceneBodyPolicy(bodies[1]).Value().linearVelocity.x == 1);
        REQUIRE(world->TickStatistics().pendingCommands == 0);
    }

    TEST_CASE("Containment preserves prior publication and settles pending queries", "[physics][native][nonfinite]") {
        const auto policy = GENERATE(PhysicsNonFinitePolicy::QuarantineBody, PhysicsNonFinitePolicy::FailWorld);
        const bool resetBeforeTeardown = GENERATE(false, true);
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto settings = Test::SmallWorldSettings().Values();
        settings.nonFinitePolicy = policy;
        auto world = runtime->PrepareWorld(PhysicsWorldSettings::Capture(settings).Value()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(936).Value()).HasValue());
        const ShapeHandle shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        PhysicsBodyDescriptor descriptor;
        descriptor.shape = shape;
        const BodyHandle first = world->CreateSceneBody({descriptor, false}).Value();
        descriptor.pose.translation.x = 4;
        const BodyHandle second = world->CreateSceneBody({descriptor, false}).Value();
        const PhysicsFixedTickInput tick{.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)};
        REQUIRE(world->AdvanceFixedTick(tick).HasValue());
        const auto prior = world->PublishedTick();
        auto capability = world->IssueQueryEventCapability().Value();
        PhysicsQueryDescriptor query;
        query.world = world->Identity();
        query.sceneGeneration = 7;
        query.geometry = PhysicsRayQuery{.maximumDistanceMeters = 10};
        query.filter.channel = PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value();
        const PhysicsQueryCommand command{.identity = capability.Identity(),
                                          .expectedPublicationRevision = prior.publicationRevision,
                                          .descriptor = query};
        auto batch = capability.SubmitBatch(std::array{command}).Value();
        REQUIRE(PhysicsWorldContainmentTestAccess::Inject(*world, first, std::numeric_limits<float>::quiet_NaN()));
        REQUIRE(PhysicsWorldContainmentTestAccess::Inject(*world, second, -std::numeric_limits<float>::infinity()));
        auto next = tick;
        next.simulationTick = 2;
        const auto outcome = world->AdvanceFixedTick(next);
        if (policy == PhysicsNonFinitePolicy::QuarantineBody) {
            REQUIRE(outcome.HasValue());
            Test::RequireError(world->ReadSceneBodyPolicy(first), PhysicsErrors::HandleStale);
            Test::RequireError(world->ReadSceneBodyPolicy(second), PhysicsErrors::HandleStale);
            Test::RequireError(batch.Poll(), PhysicsErrors::QuerySnapshotStale);
            REQUIRE(world->PublishedTick().completedTick == 2);
        } else {
            Test::RequireError(outcome, PhysicsErrors::BodyStateNonFinite);
            Test::RequireError(batch.Poll(), PhysicsErrors::CapabilityStale);
            RequireTerminalRecovery(*world, next, prior, resetBeforeTeardown);
        }
    }

    TEST_CASE("Non-finite post-step state obeys configured quarantine or terminal world policy", "[physics][native][nonfinite]") {
        const auto component = GENERATE(range(std::uint8_t{0}, std::uint8_t{19}));
        const bool postStep = GENERATE(false, true);
        for (const auto policy : {PhysicsNonFinitePolicy::QuarantineBody, PhysicsNonFinitePolicy::FailWorld}) {
            for (const float injected : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                         -std::numeric_limits<float>::infinity()}) {
                auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
                auto descriptor = Test::SmallWorldSettings().Values();
                descriptor.nonFinitePolicy = policy;
                auto world = runtime->PrepareWorld(PhysicsWorldSettings::Capture(descriptor).Value()).Value();
                const PhysicsWorldId identity = PhysicsWorldId::Create(930).Value();
                REQUIRE(world->Activate(identity).HasValue());
                const ShapeHandle shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
                PhysicsBodyDescriptor bodyDescriptor;
                bodyDescriptor.shape = shape;
                bodyDescriptor.motion = PhysicsMotionType::Dynamic;
                bodyDescriptor.mass = PhysicsMass{2.0F};
                const BodyHandle corrupt = world->CreateSceneBody({.body = bodyDescriptor, .sensor = false, .sceneEntity = 777}).Value();
                bodyDescriptor.pose.translation.x = 4.0F;
                const BodyHandle healthy = world->CreateSceneBody({.body = bodyDescriptor, .sensor = false, .sceneEntity = 778}).Value();
                PhysicsConstraintDescriptor constraint;
                constraint.first = {corrupt, {}};
                constraint.second = PhysicsWorldAnchor{};
                constraint.parameters = PhysicsFixedConstraint{};
                REQUIRE(world->QueueStructuralCommand(FutureMutation(corrupt)).HasValue());
                REQUIRE(world->CreateSceneConstraint(constraint).HasValue());
                REQUIRE(PhysicsWorldContainmentTestAccess::Inject(*world, corrupt, injected, component, postStep));

                const PhysicsFixedTickInput tick{.simulationTick = 1,
                                                 .sceneGeneration = 7,
                                                 .fixedDelta = Duration::FromNanoseconds(16'666'667)};
                const auto stepped = world->AdvanceFixedTick(tick);
                if (stepped.HasError())
                    INFO(stepped.ErrorValue().message);
                RequireNonFiniteDiagnostic(*world, identity, corrupt);
                if (policy == PhysicsNonFinitePolicy::QuarantineBody) {
                    REQUIRE(stepped.HasValue());
                    RequireQuarantineOutcome(*world, corrupt, healthy);
                } else {
                    RequireFailedWorldOutcome(*world, stepped, tick);
                }
            }
        }
    }
#endif
}  // namespace Horo::Physics
