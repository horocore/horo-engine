#include "PhysicsStateWorldTestSupport.h"

#include <algorithm>

namespace Horo::Physics::StateTest {
    namespace {
        void Advance(PhysicsWorld &world, std::uint32_t count) {
            for (std::uint32_t index = 0; index < count; ++index)
                Check(world.AdvanceFixedTick({.simulationTick = world.PublishedTick().completedTick + 1,
                                              .sceneGeneration = 7,
                                              .fixedDelta = Duration::FromNanoseconds(16'666'667)}));
        }

        void ChangeFloor(PhysicsWorld &world, BodyHandle body, PhysicsMotionType motion) {
            const auto tick = world.PublishedTick().completedTick + 1;
            const PhysicsMassPolicy mass =
                motion == PhysicsMotionType::Dynamic ? PhysicsMassPolicy{PhysicsMass{37.25F}} : PhysicsMassPolicy{PhysicsNoMass{}};
            Take(world.QueueStructuralCommand({.order = {.simulationTick = tick,
                                                         .worldGeneration = body.world.Value(),
                                                         .sceneGeneration = 7,
                                                         .targetKind = PhysicsCommandTargetKind::Body,
                                                         .targetIdentity = body.slot.index + 1ULL,
                                                         .commandKind = PhysicsStructuralCommandKind::Change,
                                                         .source = Take(PhysicsCommandSourceId::Create(10)),
                                                         .sourceSequence = 1},
                                               .bodyMutation = PhysicsBodyMutation{.body = body, .motion = motion, .mass = mass}}));
            Advance(world, 1);
            REQUIRE(Take(world.ReadSceneBodyPolicy(body)).motion == motion);
        }

        void Transition(bool kinematic) {
            WorldFixture source;
            auto receiver = Take(source.runtime->PrepareWorld(source.world->Settings(), {source.schema, 1, {}}));
            Check(receiver->Activate(Take(PhysicsWorldId::Create(80))));
            const auto layout = PopulateStateWorld(*receiver);
            Advance(*receiver, 5);
            if (kinematic) {
                ChangeFloor(*source.world, source.bodies[0].body, PhysicsMotionType::Kinematic);
                ChangeFloor(*receiver, layout.bodies[0].body, PhysicsMotionType::Kinematic);
            }
            static_cast<void>(source.Configure());
            auto checkpoint = source.Capture();
            auto configuration = source.Configuration();
            configuration.bodies = layout.bodies;
            configuration.shapes = layout.shapes;
            Take(receiver->ConfigureStateSnapshots(configuration));
            auto decoded = Take(receiver->DecodeStateSnapshot(checkpoint->Bytes()));
            auto candidate = Take(receiver->PrepareStateRestore(decoded));
            Check(candidate.ValidatePublication());
            candidate.Publish();
            ChangeFloor(*source.world, source.bodies[0].body, PhysicsMotionType::Dynamic);
            ChangeFloor(*receiver, layout.bodies[0].body, PhysicsMotionType::Dynamic);
            static_cast<void>(source.Configure());
            Take(receiver->ConfigureStateSnapshots(configuration));
            const auto expected = source.Capture();
            const auto actual = Take(receiver->CaptureState());
            CHECK(std::ranges::equal(actual->Bytes(), expected->Bytes()));
        }
    }  // namespace

    TEST_CASE("Real restored reserved-static inertia is overwritten before its first dynamic solver step",
              "[physics][native][state][inertia][assert-enabled-profile]") {
        Transition(false);
    }

    TEST_CASE("Real restored kinematic reserved inertia is overwritten before its first dynamic solver step",
              "[physics][native][state][inertia][assert-enabled-profile]") {
        Transition(true);
    }
}  // namespace Horo::Physics::StateTest
