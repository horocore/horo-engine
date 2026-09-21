#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>

#if HORO_TEST_PHYSICS_NATIVE
namespace Horo::Physics {
    namespace {
        [[nodiscard]] PhysicsStructuralCommand SolverCommand(const std::uint64_t tick, const std::uint64_t world, const std::uint64_t scene,
                                                             const std::uint64_t sourceSequence, const std::uint64_t subject,
                                                             const PhysicsStructuralCommandKind kind) {
            PhysicsStructuralCommand command;
            command.order.simulationTick = tick;
            command.order.worldGeneration = world;
            command.order.sceneGeneration = scene;
            command.order.targetKind = PhysicsCommandTargetKind::Body;
            command.order.targetIdentity = subject;
            command.order.commandKind = kind;
            command.order.source = PhysicsCommandSourceId::Create(1).Value();
            command.order.sourceSequence = sourceSequence;
            return command;
        }

        struct SolverTrace final {
            std::atomic_uint32_t completed{};
            std::atomic_bool fail{};
        };

        Result<void> RunSolver(void *context, const CancellationToken &) noexcept {
            auto &trace = *static_cast<SolverTrace *>(context);
            trace.completed.fetch_add(1, std::memory_order_release);
            if (trace.fail.load(std::memory_order_acquire))
                return Result<void>::Failure(MakeError(PhysicsErrors::InitializationFailed, "Injected solver worker failure."));
            return Result<void>::Success();
        }

        void RequireSolverPublication(const PhysicsPublishedTick &published) {
            REQUIRE(published.completedTick == 1);
            REQUIRE(published.publicationRevision == 1);
            REQUIRE(published.transformTick == published.completedTick);
            REQUIRE(published.queryTick == published.completedTick);
            REQUIRE(published.eventTick == published.completedTick);
            REQUIRE(published.appliedCommands == 0);
            REQUIRE(published.eventCount == 0);
            REQUIRE(published.droppedEventCount == 0);
        }

        void RequireFailedSolverWorld(PhysicsWorld &world, const Duration fixedDelta) {
            REQUIRE(world.State() == PhysicsWorldState::Failed);
            REQUIRE(world.LifecycleCause() == PhysicsWorldLifecycleCause::FatalSolverError);
            REQUIRE(world.LastFailure().has_value());
            REQUIRE(world.LastFailure()->code.Value() == PhysicsErrors::InitializationFailed.code.Value());
            REQUIRE(world.LastDiagnostic().has_value());
            REQUIRE(world.LastDiagnostic()->code.Value() == PhysicsErrors::InitializationFailed.code.Value());
            REQUIRE(world.LastDiagnostic()->category == PhysicsDiagnosticCategory::Runtime);
            REQUIRE(world.LastDiagnostic()->contextCount == 3);
            REQUIRE(world.PublishedTick().completedTick == 1);
            REQUIRE(
                world.AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 1, .fixedDelta = fixedDelta}).ErrorValue().code.Value() ==
                PhysicsErrors::InvalidState.code.Value());
            REQUIRE(world.Reset().HasValue());
            REQUIRE(world.State() == PhysicsWorldState::PreparedSolver);
            REQUIRE(world.LifecycleCause() == PhysicsWorldLifecycleCause::Reset);
            REQUIRE_FALSE(world.LastFailure().has_value());
            REQUIRE_FALSE(world.LastDiagnostic().has_value());
            REQUIRE_FALSE(world.Identity().IsValid());
            REQUIRE(world.PublishedTick().publicationRevision == 0);
            REQUIRE(world.TickStatistics().pendingCommands == 0);
        }
    }  // namespace

    TEST_CASE("Physics joins solver jobs before publication and enters one terminal failure state", "[physics][tick][jobs]") {
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 4});
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical, &jobs).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(202).Value()).HasValue());

        SolverTrace failingTrace;
        SolverTrace siblingTrace;
        const std::array solverJobs{PhysicsSolverJob{.context = &failingTrace, .execute = RunSolver},
                                    PhysicsSolverJob{.context = &siblingTrace, .execute = RunSolver}};
        constexpr Duration fixedDelta = Duration::FromNanoseconds(16'666'667);
        const PhysicsSolverJobBatch batch{.jobs = solverJobs.data(),
                                          .jobCount = static_cast<std::uint32_t>(solverJobs.size()),
                                          .joinTimeout = Duration::FromMilliseconds(500)};
        REQUIRE(
            world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = fixedDelta, .solverJobs = batch}).HasValue());
        REQUIRE(failingTrace.completed.load(std::memory_order_acquire) == 1);
        REQUIRE(siblingTrace.completed.load(std::memory_order_acquire) == 1);
        RequireSolverPublication(world->PublishedTick());

        failingTrace.fail.store(true, std::memory_order_release);
        REQUIRE(world->QueueStructuralCommand(SolverCommand(2, 202, 1, 1, 9, PhysicsStructuralCommandKind::Destroy)).HasValue());
        const auto failed =
            world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 1, .fixedDelta = fixedDelta, .solverJobs = batch});
        REQUIRE(failed.HasError());
        REQUIRE(failed.ErrorValue().code.Value() == PhysicsErrors::InitializationFailed.code.Value());
        REQUIRE(failingTrace.completed.load(std::memory_order_acquire) == 2);
        REQUIRE(siblingTrace.completed.load(std::memory_order_acquire) == 2);
        RequireFailedSolverWorld(*world, fixedDelta);
        REQUIRE(world->Activate(PhysicsWorldId::Create(205).Value()).HasValue());
        world->Shutdown();
        REQUIRE(world->LifecycleCause() == PhysicsWorldLifecycleCause::ProcessShutdown);
        world.reset();
        runtime.reset();
        jobs.Shutdown(ShutdownPolicy::Cancel);
    }
}  // namespace Horo::Physics
#endif
