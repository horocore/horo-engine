#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics {
#if HORO_TEST_PHYSICS_NATIVE
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
