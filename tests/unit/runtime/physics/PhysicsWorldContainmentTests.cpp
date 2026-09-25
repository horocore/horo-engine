#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"
#include "PhysicsWorldInternal.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Physics {
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        /** @brief Queues the next-tick mutation that quarantine must discard. */
        [[nodiscard]] PhysicsStructuralCommand FutureMutation(const BodyHandle body) {
            return {.order = {.simulationTick = 2,
                              .worldGeneration = 930,
                              .sceneGeneration = 7,
                              .targetKind = PhysicsCommandTargetKind::Body,
                              .targetIdentity = static_cast<std::uint64_t>(body.slot.index) + 1,
                              .commandKind = PhysicsStructuralCommandKind::Change,
                              .source = PhysicsCommandSourceId::Create(1).Value(),
                              .sourceSequence = 1},
                    .bodyMutation = PhysicsBodyMutation{.body = body, .wake = PhysicsBodyWakePolicy::Wake}};
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

    TEST_CASE("Non-finite post-step state obeys configured quarantine or terminal world policy", "[physics][native][nonfinite]") {
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
                const BodyHandle corrupt = world->CreateSceneBody({bodyDescriptor, false, 777}).Value();
                bodyDescriptor.pose.translation.x = 4.0F;
                const BodyHandle healthy = world->CreateSceneBody({bodyDescriptor, false, 778}).Value();
                PhysicsConstraintDescriptor constraint;
                constraint.first = {corrupt, {}};
                constraint.second = PhysicsWorldAnchor{};
                constraint.parameters = PhysicsFixedConstraint{};
                REQUIRE(world->QueueStructuralCommand(FutureMutation(corrupt)).HasValue());
                REQUIRE(world->CreateSceneConstraint(constraint).HasValue());
                REQUIRE(PhysicsWorldContainmentTestAccess::Inject(*world, corrupt, injected));

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
