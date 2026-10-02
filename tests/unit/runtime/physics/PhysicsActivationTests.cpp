#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Physics {
    namespace {
        /** @brief Executes one exact fixed tick through the public owner boundary. */
        void Tick(PhysicsWorld &world, const std::uint64_t tick) {
            REQUIRE(
                world.AdvanceFixedTick({.simulationTick = tick, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        }

        /** @brief Queues wake-only intent without changing policy or native state at admission. */
        [[nodiscard]] PhysicsStructuralCommand WakeCommand(const BodyHandle body, const std::uint64_t tick) {
            return {.order = {.simulationTick = tick,
                              .worldGeneration = body.world.Value(),
                              .sceneGeneration = 7,
                              .targetKind = PhysicsCommandTargetKind::Body,
                              .targetIdentity = static_cast<std::uint64_t>(body.slot.index) + 1,
                              .commandKind = PhysicsStructuralCommandKind::Change,
                              .source = PhysicsCommandSourceId::Create(1).Value(),
                              .sourceSequence = 1},
                    .bodyMutation = PhysicsBodyMutation{.body = body, .wake = PhysicsBodyWakePolicy::Wake}};
        }

        /** @brief Admits copied wake intent to the owning world's bounded command queue. */
        void QueueWake(PhysicsWorld &world, const BodyHandle body, const std::uint64_t tick) {
            REQUIRE(world.QueueStructuralCommand(WakeCommand(body, tick)).HasValue());
        }
    }  // namespace

    TEST_CASE("Null activation observations fail explicitly through lifecycle and owner boundaries", "[physics][activation]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Null).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        Test::RequireError(world->ReadSceneActivation(), PhysicsErrors::InvalidState);
        REQUIRE(world->Activate(PhysicsWorldId::Create(8620).Value()).HasValue());
        Test::RequireError(world->ReadSceneActivation(), PhysicsErrors::CapabilityUnavailable);
        bool rejected = false;
        std::thread foreign([&] {
            const auto result = world->ReadSceneActivation();
            rejected = result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(rejected);
        world->Shutdown();
        Test::RequireError(world->ReadSceneActivation(), PhysicsErrors::InvalidState);
    }

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("New dynamic bodies start awake and respond to world gravity", "[physics][native][activation]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        const auto owner = PhysicsWorldId::Create(8621).Value();
        REQUIRE(world->Activate(owner).HasValue());
        const auto empty = world->ReadSceneActivation().Value();
        REQUIRE(empty.world == owner);
        REQUIRE(empty.awakeMovingBodies == 0);
        REQUIRE(empty.sleepingMovingBodies == 0);
        REQUIRE_FALSE(empty.islands.has_value());
        PhysicsBodyDescriptor descriptor;
        descriptor.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        descriptor.motion = PhysicsMotionType::Dynamic;
        descriptor.mass = PhysicsMass{1.0F};
        const auto body = world->CreateSceneBody({descriptor}).Value();
        REQUIRE(world->ReadSceneBodyReconciliation(body).Value().state.activity == PhysicsBodyActivity::Awake);
        Tick(*world, 1);
        const auto state = world->ReadSceneBodyReconciliation(body).Value().state;
        REQUIRE(state.linearVelocity.y < 0.0F);
        REQUIRE(state.pose.translation.y < 0.0F);
        REQUIRE(world->ReadSceneActivation().Value().awakeMovingBodies == 1);
        bool rejected = false;
        std::thread foreign([&] {
            const auto result = world->ReadSceneActivation();
            rejected = result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(rejected);
        runtime->Shutdown();
        Test::RequireError(world->ReadSceneActivation(), PhysicsErrors::InvalidState);
    }

    TEST_CASE("Sleeping bodies wake only at the named tick and settle under the captured policy", "[physics][native][activation]") {
        auto settings = Test::SmallWorldSettings().Values();
        settings.world.gravity = {};
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(PhysicsWorldSettings::Capture(settings).Value()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8622).Value()).HasValue());
        PhysicsBodyDescriptor descriptor;
        descriptor.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        descriptor.mass = PhysicsNoMass{};
        const auto fixed = world->CreateSceneBody({descriptor}).Value();
        REQUIRE(world->ReadSceneBodyReconciliation(fixed).Value().state.activity == PhysicsBodyActivity::Static);
        descriptor.motion = PhysicsMotionType::Dynamic;
        descriptor.mass = PhysicsMass{1.0F};
        const auto body = world->CreateSceneBody({descriptor, false, PhysicsInitialBodyActivity::Sleeping}).Value();
        const auto initial = world->ReadSceneActivation().Value();
        REQUIRE(initial.staticBodies == 1);
        REQUIRE(initial.sleepingMovingBodies == 1);
        QueueWake(*world, body, 2);
        Tick(*world, 1);
        REQUIRE(world->ReadSceneBodyReconciliation(body).Value().state.activity == PhysicsBodyActivity::Sleeping);
        Tick(*world, 2);
        REQUIRE(world->ReadSceneBodyReconciliation(body).Value().state.activity == PhysicsBodyActivity::Awake);
        for (std::uint64_t tick = 3; tick <= 90; ++tick)
            Tick(*world, tick);
        REQUIRE(world->ReadSceneBodyReconciliation(body).Value().state.activity == PhysicsBodyActivity::Sleeping);
        const auto retained = world->ReadSceneActivation().Value();
        REQUIRE(retained.sleepingMovingBodies == 1);
        REQUIRE_FALSE(retained.islands.has_value());
        auto damping = WakeCommand(body, 91);
        damping.bodyMutation->wake = PhysicsBodyWakePolicy::Preserve;
        PhysicsMotionSafety safety;
        safety.linearDampingPerSecond = 1.0F;
        damping.bodyMutation->motionSafety = safety;
        REQUIRE(world->QueueStructuralCommand(damping).HasValue());
        Tick(*world, 91);
        REQUIRE(world->ReadSceneBodyReconciliation(body).Value().state.activity == PhysicsBodyActivity::Sleeping);
        QueueWake(*world, body, 92);
        Tick(*world, 92);
        QueueWake(*world, body, 93);
        Tick(*world, 93);
        REQUIRE(world->ReadSceneActivation().Value().awakeMovingBodies == 1);
        Test::RequireError(world->QueueStructuralCommand(WakeCommand(fixed, 94)), PhysicsErrors::OperationUnsupported);
        REQUIRE(world->Reset().HasValue());
        Test::RequireError(world->ReadSceneActivation(), PhysicsErrors::InvalidState);
        REQUIRE(world->Activate(PhysicsWorldId::Create(8623).Value()).HasValue());
        REQUIRE(world->ReadSceneActivation().Value().sleepingMovingBodies == 0);
        Test::RequireError(world->ReadSceneBodyReconciliation(body), PhysicsErrors::HandleWorldMismatch);
        REQUIRE(retained.sleepingMovingBodies == 1);
    }

    TEST_CASE("Kinematic activity is distinct from static bodies and solver gravity", "[physics][native][activation]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8626).Value()).HasValue());
        PhysicsBodyDescriptor descriptor;
        descriptor.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        descriptor.motion = PhysicsMotionType::Kinematic;
        descriptor.mass = PhysicsNoMass{};
        descriptor.linearVelocity.x = 1.0F;
        const auto body = world->CreateSceneBody({descriptor}).Value();
        Tick(*world, 1);
        const auto state = world->ReadSceneBodyReconciliation(body).Value().state;
        REQUIRE(state.activity == PhysicsBodyActivity::Awake);
        REQUIRE(state.pose.translation.x > 0.0F);
        REQUIRE(state.pose.translation.y == 0.0F);
        REQUIRE(world->ReadSceneActivation().Value().staticBodies == 0);
    }

    TEST_CASE("Initial sleep rejects unsafe or unknown requests without admitting a body", "[physics][native][activation]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8624).Value()).HasValue());
        PhysicsSceneBodyDescriptor descriptor;
        descriptor.body.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        descriptor.body.mass = PhysicsNoMass{};
        descriptor.initialActivity = PhysicsInitialBodyActivity::Sleeping;
        Test::RequireError(world->CreateSceneBody(descriptor), PhysicsErrors::OperationUnsupported);
        descriptor.body.motion = PhysicsMotionType::Dynamic;
        descriptor.body.mass = PhysicsMass{1.0F};
        descriptor.body.linearVelocity.x = 0.1F;
        Test::RequireError(world->CreateSceneBody(descriptor), PhysicsErrors::DescriptorInvalid);
        descriptor.body.linearVelocity = {};
        descriptor.body.angularVelocity.y = 0.1F;
        Test::RequireError(world->CreateSceneBody(descriptor), PhysicsErrors::DescriptorInvalid);
        descriptor.body.angularVelocity = {};
        descriptor.initialActivity = static_cast<PhysicsInitialBodyActivity>(255);
        Test::RequireError(world->CreateSceneBody(descriptor), PhysicsErrors::OperationUnsupported);
        REQUIRE(world->ReadSceneActivation().Value().awakeMovingBodies == 0);
        REQUIRE(world->ReadSceneActivation().Value().sleepingMovingBodies == 0);
    }

    TEST_CASE("Wake-only commands support constrained sleeping bodies without changing joint ownership", "[physics][native][activation]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8625).Value()).HasValue());
        PhysicsBodyDescriptor descriptor;
        descriptor.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        descriptor.motion = PhysicsMotionType::Dynamic;
        descriptor.mass = PhysicsMass{1.0F};
        const auto first = world->CreateSceneBody({descriptor, false, PhysicsInitialBodyActivity::Sleeping}).Value();
        descriptor.pose.translation.x = 2.0F;
        const auto second = world->CreateSceneBody({descriptor, false, PhysicsInitialBodyActivity::Sleeping}).Value();
        PhysicsConstraintDescriptor joint;
        joint.first = {first, {}};
        joint.second = PhysicsBodyAnchor{second, {}};
        joint.parameters = PhysicsDistanceConstraint{1.0F, 3.0F};
        const auto constraint = world->CreateSceneConstraint(joint).Value();
        QueueWake(*world, first, 1);
        REQUIRE(world->ReadSceneBodyReconciliation(first).Value().state.activity == PhysicsBodyActivity::Sleeping);
        Tick(*world, 1);
        REQUIRE(world->ReadSceneBodyReconciliation(first).Value().state.activity == PhysicsBodyActivity::Awake);
        REQUIRE(world->ReadSceneBodyReconciliation(second).Value().state.activity == PhysicsBodyActivity::Awake);
        REQUIRE(world->DestroySceneConstraint(constraint).HasValue());
    }
#endif
}  // namespace Horo::Physics
