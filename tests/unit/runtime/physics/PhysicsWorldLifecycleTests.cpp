#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"
#include "PhysicsWorldInternal.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Physics {
    namespace {
        void RequirePreparedNullWorld(const PhysicsWorld &world) {
            REQUIRE(world.State() == PhysicsWorldState::PreparedNull);
            REQUIRE_FALSE(world.Identity().IsValid());
            REQUIRE(world.LifecycleCause() == PhysicsWorldLifecycleCause::Reset);
        }

        [[nodiscard]] PhysicsStructuralCommand DestroyCommand() {
            return {.order = {.simulationTick = 1,
                              .worldGeneration = 100,
                              .sceneGeneration = 1,
                              .targetKind = PhysicsCommandTargetKind::Body,
                              .targetIdentity = 1,
                              .commandKind = PhysicsStructuralCommandKind::Destroy,
                              .source = PhysicsCommandSourceId::Create(1).Value(),
                              .sourceSequence = 1}};
        }
    }  // namespace

    TEST_CASE("Null Physics is explicit omitted capability and never invents simulation", "[physics][lifecycle]") {
        auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Null);
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        REQUIRE(runtime->Mode() == PhysicsRuntimeMode::Null);
        REQUIRE(runtime->State() == PhysicsRuntimeState::Ready);
        REQUIRE(runtime->Availability() == PhysicsAvailability::Omitted);
        for (std::uint8_t value = 0; value < static_cast<std::uint8_t>(PhysicsCapability::Count); ++value)
            REQUIRE(runtime->Capability(static_cast<PhysicsCapability>(value)) == PhysicsCapabilitySupport::Unsupported);
        REQUIRE(runtime->Capability(static_cast<PhysicsCapability>(255)) == PhysicsCapabilitySupport::Unknown);
        const auto settings = Test::SmallWorldSettings();
        auto prepared = runtime->PrepareWorld(settings);
        REQUIRE(prepared.HasValue());
        auto world = std::move(prepared).Value();
        REQUIRE(world->State() == PhysicsWorldState::PreparedNull);
        REQUIRE_FALSE(world->Identity().IsValid());
        REQUIRE(world->Settings().Identity() == settings.Identity());
        REQUIRE(world->Activate({}).ErrorValue().code.Value() == PhysicsErrors::WorldInvalid.code.Value());
        REQUIRE(world->State() == PhysicsWorldState::PreparedNull);
        const auto identity = PhysicsWorldId::Create(100).Value();
        REQUIRE(world->Activate(identity).HasValue());
        REQUIRE(world->State() == PhysicsWorldState::ActiveNull);
        const auto destroyCommand = DestroyCommand();
        REQUIRE(world->QueueStructuralCommand(destroyCommand).ErrorValue().code.Value() ==
                PhysicsErrors::CapabilityUnavailable.code.Value());
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .ErrorValue()
                    .code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
        REQUIRE(world->PublishedTick().publicationRevision == 0);
        REQUIRE(world->Identity() == identity);
        REQUIRE(world->Activate(identity).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        REQUIRE(world->LifecycleCause() == PhysicsWorldLifecycleCause::None);
        REQUIRE_FALSE(world->LastFailure().has_value());
        REQUIRE(world->Reset().HasValue());
        REQUIRE(world->Reset().HasValue());
        RequirePreparedNullWorld(*world);
        REQUIRE(world->Activate(PhysicsWorldId::Create(106).Value()).HasValue());
        REQUIRE(world->UnloadScene().HasValue());
        REQUIRE(world->UnloadScene().HasValue());
        REQUIRE(world->State() == PhysicsWorldState::Destroyed);
        REQUIRE(world->Identity() == PhysicsWorldId::Create(106).Value());
        REQUIRE(world->LifecycleCause() == PhysicsWorldLifecycleCause::SceneUnload);
        world->Shutdown();
        REQUIRE(world->LifecycleCause() == PhysicsWorldLifecycleCause::SceneUnload);
        REQUIRE(world->Reset().ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        runtime->Shutdown();
        runtime->Shutdown();
        REQUIRE(runtime->State() == PhysicsRuntimeState::Stopped);
        REQUIRE(runtime->PrepareWorld(settings).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
    }

    TEST_CASE("Null Physics rejects scene admission as unavailable", "[physics][lifecycle][scene]") {
        auto runtime = std::move(PhysicsRuntime::Create(PhysicsRuntimeMode::Null).Value());
        auto world = std::move(runtime->PrepareWorld(Test::SmallWorldSettings()).Value());
        REQUIRE(world->Activate(PhysicsWorldId::Create(100).Value()).HasValue());
        const PhysicsShapeDescriptor sceneShape = PhysicsBoxShape{};
        const std::span<const PhysicsSceneShapeInstance> emptyInstances{};
        REQUIRE(world->CreateSceneShape(sceneShape).ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
        REQUIRE(world->CreateSceneCompoundShape(emptyInstances).ErrorValue().code.Value() ==
                PhysicsErrors::CapabilityUnavailable.code.Value());
        REQUIRE(world->CreateSceneBody({}).ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
        REQUIRE(world->ReadSceneBodyPolicy({}).ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
        REQUIRE(world->CreateSceneConstraint({}).ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
    }

    TEST_CASE("Physics rejects unknown compositions and closes unactivated candidates on runtime shutdown", "[physics][lifecycle]") {
        const auto unknown = PhysicsRuntime::Create(static_cast<PhysicsRuntimeMode>(255));
        REQUIRE(unknown.HasError());
        REQUIRE(unknown.ErrorValue().code.Value() == PhysicsErrors::OperationUnsupported.code.Value());
        auto runtime = std::move(PhysicsRuntime::Create(PhysicsRuntimeMode::Null).Value());
        auto candidate = std::move(runtime->PrepareWorld(Test::SmallWorldSettings()).Value());
        runtime->Shutdown();
        REQUIRE(candidate->Activate(PhysicsWorldId::Create(101).Value()).ErrorValue().code.Value() ==
                PhysicsErrors::InvalidState.code.Value());
        runtime.reset();
        candidate->Shutdown();
        REQUIRE(candidate->State() == PhysicsWorldState::Destroyed);
    }

    TEST_CASE("Physics preparation and activation reject a foreign owner thread", "[physics][lifecycle]") {
        auto runtime = std::move(PhysicsRuntime::Create(PhysicsRuntimeMode::Null).Value());
        const auto settings = Test::SmallWorldSettings();
        auto candidate = std::move(runtime->PrepareWorld(settings).Value());
        bool rejectedPreparation = false;
        bool rejectedActivation = false;
        bool rejectedReset = false;
        bool rejectedUnload = false;
        std::thread foreign([&] {
            const auto prepared = runtime->PrepareWorld(settings);
            rejectedPreparation =
                prepared.HasError() && prepared.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
            const auto activated = candidate->Activate(PhysicsWorldId::Create(102).Value());
            rejectedActivation =
                activated.HasError() && activated.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
            const auto reset = candidate->Reset();
            rejectedReset = reset.HasError() && reset.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
            const auto unloaded = candidate->UnloadScene();
            rejectedUnload =
                unloaded.HasError() && unloaded.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(rejectedPreparation);
        REQUIRE(rejectedActivation);
        REQUIRE(rejectedReset);
        REQUIRE(rejectedUnload);
        REQUIRE(candidate->State() == PhysicsWorldState::PreparedNull);
    }

    TEST_CASE("Physics rejects duplicate active world identity without changing either world", "[physics][lifecycle]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Null).Value();
        const auto settings = Test::SmallWorldSettings();
        auto first = runtime->PrepareWorld(settings).Value();
        auto second = runtime->PrepareWorld(settings).Value();
        const auto identity = PhysicsWorldId::Create(105).Value();
        REQUIRE(first->Activate(identity).HasValue());
        const auto duplicate = second->Activate(identity);
        REQUIRE(duplicate.HasError());
        REQUIRE(duplicate.ErrorValue().code.Value() == PhysicsErrors::WorldInvalid.code.Value());
        REQUIRE(first->State() == PhysicsWorldState::ActiveNull);
        REQUIRE(second->State() == PhysicsWorldState::PreparedNull);
        REQUIRE_FALSE(second->Identity().IsValid());
    }

    TEST_CASE("Canonical Physics composition is explicit and world ownership survives owner shutdown", "[physics][lifecycle]") {
        auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
#if HORO_TEST_PHYSICS_NATIVE
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        REQUIRE(runtime->Availability() == PhysicsAvailability::Available);
        REQUIRE(runtime->Capability(PhysicsCapability::WorldCreation) == PhysicsCapabilitySupport::Available);
        REQUIRE(runtime->Capability(PhysicsCapability::RigidBodies) == PhysicsCapabilitySupport::Available);
        const auto duplicate = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
        REQUIRE(duplicate.HasError());
        REQUIRE(duplicate.ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        auto first = std::move(runtime->PrepareWorld(Test::SmallWorldSettings()).Value());
        auto second = std::move(runtime->PrepareWorld(Test::SmallWorldSettings()).Value());
        REQUIRE(first->State() == PhysicsWorldState::PreparedSolver);
        REQUIRE(second->State() == PhysicsWorldState::PreparedSolver);
        REQUIRE(first->Activate(PhysicsWorldId::Create(103).Value()).HasValue());
        REQUIRE(second->Activate(PhysicsWorldId::Create(104).Value()).HasValue());
        REQUIRE(first->Identity() != second->Identity());
        first->Shutdown();
        REQUIRE(second->State() == PhysicsWorldState::ActiveSolver);
        runtime->Shutdown();
        REQUIRE(runtime->Availability() == PhysicsAvailability::Unavailable);
        REQUIRE(runtime->Capability(PhysicsCapability::WorldCreation) == PhysicsCapabilitySupport::Unavailable);
        runtime.reset();
        REQUIRE(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).HasError());
        second->Shutdown();
        REQUIRE(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).HasValue());
#else
        REQUIRE(created.HasError());
        REQUIRE(created.ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
#endif
    }

#if HORO_TEST_PHYSICS_NATIVE
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
        for (const float value : values) {
            descriptor.pose.translation.x = value;
            Test::RequireError(world->CreateSceneBody({descriptor, false}), PhysicsErrors::DescriptorInvalid);
            descriptor.pose.translation.x = 0.0F;
            descriptor.angularVelocity.z = value;
            Test::RequireError(world->CreateSceneBody({descriptor, false}), PhysicsErrors::DescriptorInvalid);
            descriptor.angularVelocity.z = 0.0F;
        }
        const BodyHandle body = world->CreateSceneBody({descriptor, false}).Value();
        PhysicsStructuralCommand command{.order = {.simulationTick = 1,
                                                   .worldGeneration = 929,
                                                   .sceneGeneration = 7,
                                                   .targetKind = PhysicsCommandTargetKind::Body,
                                                   .targetIdentity = static_cast<std::uint64_t>(body.slot.index) + 1,
                                                   .commandKind = PhysicsStructuralCommandKind::Change,
                                                   .source = PhysicsCommandSourceId::Create(1).Value(),
                                                   .sourceSequence = 1},
                                         .bodyMutation = PhysicsBodyMutation{.body = body}};
        for (const float value : values) {
            command.bodyMutation->linearVelocity = Math::Vec3{value, 0.0F, 0.0F};
            Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::DescriptorInvalid);
        }
        REQUIRE(world->State() == PhysicsWorldState::ActiveSolver);
        REQUIRE(world->PublishedTick().publicationRevision == 0);
    }

#endif

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Canonical scene admission enforces lifecycle, ownership and descriptor boundaries", "[physics][native][scene]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        const PhysicsShapeDescriptor sceneShape = PhysicsBoxShape{};
        const std::span<const PhysicsSceneShapeInstance> emptyInstances{};
        REQUIRE(world->CreateSceneShape(sceneShape).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        REQUIRE(world->CreateSceneCompoundShape(emptyInstances).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        REQUIRE(world->CreateSceneBody({}).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        REQUIRE(world->CreateSceneConstraint({}).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());

        const auto identity = PhysicsWorldId::Create(107).Value();
        REQUIRE(world->Activate(identity).HasValue());
        REQUIRE(world->CreateSceneShape(PhysicsBoxShape{{0.0F, 0.5F, 0.5F}}).ErrorValue().code.Value() ==
                PhysicsErrors::DescriptorInvalid.code.Value());
        const auto shape = world->CreateSceneShape(sceneShape);
        REQUIRE(shape.HasValue());
        REQUIRE(world->CreateSceneCompoundShape(emptyInstances).ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());

        const PhysicsSceneShapeInstance foreign{{PhysicsWorldId::Create(108).Value(), {0, 1}}, {}};
        const std::array foreignInstances{foreign};
        REQUIRE(world->CreateSceneCompoundShape(foreignInstances).ErrorValue().code.Value() ==
                PhysicsErrors::HandleWorldMismatch.code.Value());
        const PhysicsSceneShapeInstance local{shape.Value(), {}};
        const std::array localInstances{local};
        REQUIRE(world->CreateSceneCompoundShape(localInstances).HasValue());

        PhysicsBodyDescriptor body;
        body.shape = shape.Value();
        body.motion = PhysicsMotionType::Static;
        body.mass = PhysicsNoMass{};
        const auto bodyResult = world->CreateSceneBody({body, false});
        REQUIRE(bodyResult.HasValue());

        PhysicsConstraintDescriptor constraint;
        constraint.first = {bodyResult.Value(), {}};
        constraint.second = PhysicsWorldAnchor{};
        constraint.parameters = PhysicsFixedConstraint{};
        REQUIRE(world->CreateSceneConstraint(constraint).HasValue());
    }

    TEST_CASE("Canonical scene admission enforces owner-thread boundaries", "[physics][native][scene][thread]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(108).Value()).HasValue());
        const PhysicsShapeDescriptor sceneShape = PhysicsBoxShape{};
        const std::span<const PhysicsSceneShapeInstance> emptyInstances{};
        bool shapeRejected = false;
        bool compoundRejected = false;
        bool bodyRejected = false;
        bool constraintRejected = false;
        std::thread foreignThread([&] {
            shapeRejected =
                world->CreateSceneShape(sceneShape).ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
            compoundRejected = world->CreateSceneCompoundShape(emptyInstances).ErrorValue().code.Value() ==
                               PhysicsErrors::ThreadAffinityViolation.code.Value();
            bodyRejected = world->CreateSceneBody({}).ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
            constraintRejected =
                world->CreateSceneConstraint({}).ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreignThread.join();
        REQUIRE(shapeRejected);
        REQUIRE(compoundRejected);
        REQUIRE(bodyRejected);
        REQUIRE(constraintRejected);
    }

    TEST_CASE("Canonical body mutation reconciles mode shape and properties only at the pre-step safe point",
              "[physics][native][mutation]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(865).Value()).HasValue());
        const ShapeHandle box = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        const ShapeHandle sphere = world->CreateSceneShape(PhysicsSphereShape{1.25F}).Value();
        PhysicsBodyDescriptor initial;
        initial.shape = box;
        initial.mass = PhysicsNoMass{};
        const BodyHandle body = world->CreateSceneBody({initial, false}).Value();
        REQUIRE(world->ReadSceneBodyReconciliation(body).Value().observedMotion == PhysicsMotionType::Static);

        PhysicsStructuralCommand command{.order = {.simulationTick = 1,
                                                   .worldGeneration = 865,
                                                   .sceneGeneration = 7,
                                                   .targetKind = PhysicsCommandTargetKind::Body,
                                                   .targetIdentity = static_cast<std::uint64_t>(body.slot.index) + 1,
                                                   .commandKind = PhysicsStructuralCommandKind::Change,
                                                   .source = PhysicsCommandSourceId::Create(1).Value(),
                                                   .sourceSequence = 1},
                                         .bodyMutation = PhysicsBodyMutation{.body = body,
                                                                             .shape = sphere,
                                                                             .motion = PhysicsMotionType::Dynamic,
                                                                             .mass = PhysicsMass{2.0F}}};
        REQUIRE(world->QueueStructuralCommand(command).Value().status == PhysicsCommandAdmissionStatus::Deferred);
        REQUIRE(world->ReadSceneBodyPolicy(body).Value().motion == PhysicsMotionType::Static);
        REQUIRE(world->QueueStructuralCommand(command).ErrorValue().code.Value() == PhysicsErrors::CommandOrderInvalid.code.Value());
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        const auto applied = world->ReadSceneBodyPolicy(body).Value();
        const auto native = world->ReadSceneBodyReconciliation(body).Value();
        REQUIRE(applied.shape == sphere);
        REQUIRE(applied.motion == PhysicsMotionType::Dynamic);
        REQUIRE(std::get<PhysicsMass>(applied.mass).kilograms == 2.0F);
        REQUIRE(native.observedShape == sphere);
        REQUIRE(native.observedMotion == PhysicsMotionType::Dynamic);
        REQUIRE(native.observedMassKilograms.has_value());
        REQUIRE(*native.observedMassKilograms > 1.99F);
        REQUIRE(*native.observedMassKilograms < 2.01F);
        REQUIRE(native.observedBoundsExtent.x > 2.4F);
        REQUIRE(native.state.activity == PhysicsBodyActivity::Awake);
        REQUIRE(world->PublishedTick().appliedCommands == 1);

        command.order.simulationTick = 2;
        PhysicsMotionSafety safety;
        safety.linearDampingPerSecond = 1.5F;
        safety.lockedAxes = PhysicsAxisLock::RotationX;
        command.bodyMutation = PhysicsBodyMutation{.body = body,
                                                   .mass = PhysicsMass{3.0F},
                                                   .motionSafety = safety,
                                                   .linearVelocity = Math::Vec3{1.0F, 0.0F, 0.0F}};
        REQUIRE(world->QueueStructuralCommand(command).HasValue());
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        const auto changed = world->ReadSceneBodyPolicy(body).Value();
        REQUIRE(std::get<PhysicsMass>(changed.mass).kilograms == 3.0F);
        REQUIRE(changed.motionSafety.linearDampingPerSecond == 1.5F);
        REQUIRE(changed.linearVelocity.x == 1.0F);
        const auto changedNative = world->ReadSceneBodyReconciliation(body).Value();
        REQUIRE(changedNative.observedMassKilograms.has_value());
        REQUIRE(*changedNative.observedMassKilograms > 2.99F);
        REQUIRE(*changedNative.observedMassKilograms < 3.01F);
        REQUIRE(changedNative.state.linearVelocity.x > 0.0F);

        command.order.simulationTick = 3;
        command.bodyMutation = PhysicsBodyMutation{.body = body, .wake = PhysicsBodyWakePolicy::Wake};
        REQUIRE(world->QueueStructuralCommand(command).HasValue());
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 3, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        REQUIRE(world->ReadSceneBodyReconciliation(body).Value().state.activity == PhysicsBodyActivity::Awake);

        command.order.simulationTick = 4;
        command.bodyMutation = PhysicsBodyMutation{.body = body, .motion = PhysicsMotionType::Static};
        REQUIRE(world->QueueStructuralCommand(command).HasValue());
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 4, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        const auto stopped = world->ReadSceneBodyPolicy(body).Value();
        REQUIRE(stopped.motion == PhysicsMotionType::Static);
        REQUIRE(std::holds_alternative<PhysicsNoMass>(stopped.mass));
        REQUIRE(world->ReadSceneBodyReconciliation(body).Value().observedMotion == PhysicsMotionType::Static);
    }

    TEST_CASE("Canonical body mutation rejects unsupported, stale and lifecycle-edge requests without publication",
              "[physics][native][mutation][lifecycle]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(866).Value()).HasValue());
        const ShapeHandle box = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        const ShapeHandle plane = world->CreateSceneShape(PhysicsStaticPlaneShape{}).Value();
        PhysicsBodyDescriptor initial;
        initial.shape = box;
        initial.mass = PhysicsNoMass{};
        const BodyHandle body = world->CreateSceneBody({initial, false}).Value();
        PhysicsStructuralCommand command{.order = {.simulationTick = 1,
                                                   .worldGeneration = 866,
                                                   .sceneGeneration = 7,
                                                   .targetKind = PhysicsCommandTargetKind::Body,
                                                   .targetIdentity = static_cast<std::uint64_t>(body.slot.index) + 1,
                                                   .commandKind = PhysicsStructuralCommandKind::Change,
                                                   .source = PhysicsCommandSourceId::Create(1).Value(),
                                                   .sourceSequence = 1},
                                         .bodyMutation = PhysicsBodyMutation{.body = body,
                                                                             .shape = plane,
                                                                             .motion = PhysicsMotionType::Dynamic,
                                                                             .mass = PhysicsMass{2.0F}}};
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::ShapeMotionUnsupported);
        REQUIRE(world->PublishedTick().publicationRevision == 0);
        command.bodyMutation = PhysicsBodyMutation{.body = body, .linearVelocity = Math::Vec3{1.0F, 0.0F, 0.0F}};
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::DescriptorInvalid);
        command.bodyMutation = PhysicsBodyMutation{.body = body, .wake = PhysicsBodyWakePolicy::Wake};
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::OperationUnsupported);
        PhysicsMotionSafety lockedSafety;
        lockedSafety.lockedAxes = PhysicsAxisLock::All;
        command.bodyMutation = PhysicsBodyMutation{.body = body,
                                                   .motion = PhysicsMotionType::Dynamic,
                                                   .mass = PhysicsMass{2.0F},
                                                   .motionSafety = lockedSafety};
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::DescriptorInvalid);
        PhysicsMotionSafety unsupportedSafety;
        unsupportedSafety.maximumDepenetrationSpeed = 10.0F;
        command.bodyMutation = PhysicsBodyMutation{.body = body, .motionSafety = unsupportedSafety};
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::OperationUnsupported);
        command.bodyMutation =
            PhysicsBodyMutation{.body = BodyHandle{PhysicsWorldId::Create(900).Value(), body.slot}, .motion = PhysicsMotionType::Kinematic};
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::HandleWorldMismatch);
        command.bodyMutation = PhysicsBodyMutation{.body = body, .motion = PhysicsMotionType::Kinematic};
        const BodyHandle absent{world->Identity(), {body.slot.index + 1, 1}};
        command.order.targetIdentity = static_cast<std::uint64_t>(absent.slot.index) + 1;
        command.bodyMutation->body = absent;
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::HandleStale);
        command.order.targetIdentity = static_cast<std::uint64_t>(body.slot.index) + 1;
        command.bodyMutation->body = body;
        PhysicsBodyDescriptor staticPlane;
        staticPlane.shape = plane;
        staticPlane.mass = PhysicsNoMass{};
        const BodyHandle planeBody = world->CreateSceneBody({staticPlane, false}).Value();
        command.order.targetIdentity = static_cast<std::uint64_t>(planeBody.slot.index) + 1;
        command.bodyMutation =
            PhysicsBodyMutation{.body = planeBody, .shape = box, .motion = PhysicsMotionType::Dynamic, .mass = PhysicsMass{2.0F}};
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::OperationUnsupported);
        command.order.targetIdentity = static_cast<std::uint64_t>(body.slot.index) + 1;
        command.bodyMutation = PhysicsBodyMutation{.body = body, .motion = PhysicsMotionType::Kinematic};
        bool foreignRejected = false;
        bool foreignReadRejected = false;
        std::thread foreign([&] {
            foreignRejected =
                world->QueueStructuralCommand(command).ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
            foreignReadRejected =
                world->ReadSceneBodyReconciliation(body).ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(foreignRejected);
        REQUIRE(foreignReadRejected);
        REQUIRE(world->Reset().HasValue());
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::InvalidState);
        REQUIRE(world->Activate(PhysicsWorldId::Create(867).Value()).HasValue());
        Test::RequireError(world->ReadSceneBodyPolicy(body), PhysicsErrors::HandleWorldMismatch);
        runtime->Shutdown();
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::InvalidState);
        Test::RequireError(world->ReadSceneBodyReconciliation(body), PhysicsErrors::InvalidState);
        Test::RequireError(world->AdvanceFixedTick(
                               {.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)}),
                           PhysicsErrors::InvalidState);
        REQUIRE(world->UnloadScene().HasValue());
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::InvalidState);
    }

    TEST_CASE("Canonical body mutation does not edit constraint-bearing native bodies", "[physics][native][mutation]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(868).Value()).HasValue());
        const ShapeHandle shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        PhysicsBodyDescriptor descriptor;
        descriptor.shape = shape;
        descriptor.mass = PhysicsNoMass{};
        const BodyHandle body = world->CreateSceneBody({descriptor, false}).Value();
        PhysicsConstraintDescriptor constraint;
        constraint.first = {body, {}};
        constraint.second = PhysicsWorldAnchor{};
        constraint.parameters = PhysicsFixedConstraint{};
        const PhysicsStructuralCommand command{.order = {.simulationTick = 1,
                                                         .worldGeneration = 868,
                                                         .sceneGeneration = 7,
                                                         .targetKind = PhysicsCommandTargetKind::Body,
                                                         .targetIdentity = static_cast<std::uint64_t>(body.slot.index) + 1,
                                                         .commandKind = PhysicsStructuralCommandKind::Change,
                                                         .source = PhysicsCommandSourceId::Create(1).Value(),
                                                         .sourceSequence = 1},
                                               .bodyMutation = PhysicsBodyMutation{.body = body, .motion = PhysicsMotionType::Kinematic}};
        REQUIRE(world->QueueStructuralCommand(command).HasValue());
        REQUIRE(world->CreateSceneConstraint(constraint).HasValue());
        PhysicsStructuralCommand later = command;
        later.order.simulationTick = 2;
        Test::RequireError(world->QueueStructuralCommand(later), PhysicsErrors::OperationUnsupported);
        Test::RequireError(world->AdvanceFixedTick(
                               {.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)}),
                           PhysicsErrors::OperationUnsupported);
        REQUIRE(world->State() == PhysicsWorldState::ActiveSolver);
        REQUIRE(world->ReadSceneBodyReconciliation(body).Value().observedMotion == PhysicsMotionType::Static);
        REQUIRE(world->PublishedTick().publicationRevision == 0);
    }
#endif
}  // namespace Horo::Physics
