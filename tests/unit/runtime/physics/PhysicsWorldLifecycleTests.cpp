#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
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
        const PhysicsShapeDescriptor sceneShape = PhysicsBoxShape{};
        const std::span<const PhysicsSceneShapeInstance> emptyInstances{};
        REQUIRE(world->CreateSceneShape(sceneShape).ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
        REQUIRE(world->CreateSceneCompoundShape(emptyInstances).ErrorValue().code.Value() ==
                PhysicsErrors::CapabilityUnavailable.code.Value());
        REQUIRE(world->CreateSceneBody({}).ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
        REQUIRE(world->CreateSceneConstraint({}).ErrorValue().code.Value() == PhysicsErrors::CapabilityUnavailable.code.Value());
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
#endif
}  // namespace Horo::Physics
