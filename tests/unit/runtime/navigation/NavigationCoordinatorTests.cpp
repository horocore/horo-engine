#include "navigation/NavigationCoordinatorTestFixtures.h"

namespace Horo::Navigation {
    using namespace CoordinatorTestSupport;

    TEST_CASE("Coordinator rejects malformed bounds and admits no inline work", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto bad = Limits();
        bad.maximumOwnedBytes = 1;
        REQUIRE(NavigationCoordinator::Create(jobs, bad).HasError());
        bad = Limits();
        bad.requestSlots = 3;
        REQUIRE(NavigationCoordinator::Create(jobs, bad).HasError());
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world);
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
        auto invalid = Input();
        invalid.source.filterRevision = 0;
        REQUIRE(coordinator.Submit(world, invalid, 0).HasError());
        invalid = Input();
        invalid.capabilityRevision = 2;
        REQUIRE(coordinator.Submit(world, invalid, 0).HasError());
        const auto handle = Admit(coordinator, world);
        REQUIRE_FALSE(coordinator.Take(handle));
        REQUIRE(coordinator.Cancel(handle));
        const NavigationPathCaller callers[]{Caller()};
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        auto result = coordinator.Take(handle);
        REQUIRE(result);
        REQUIRE(ErrorIs(*result, NavigationErrors::QueryCancelled));
        REQUIRE_FALSE(coordinator.Take(handle));
        REQUIRE_FALSE(coordinator.Cancel(handle));
        REQUIRE(coordinator.IsDrained());
    }

    TEST_CASE("Caller quotas prevent admission and tick budget monopoly", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world);
        auto limits = Limits();
        limits.requestsPerCaller = 1;
        limits.requestsPerTick = 2;
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, limits)).Value();
        for (int index = 0; index < 4; ++index)
            static_cast<void>(Admit(coordinator, world));
        REQUIRE(coordinator.Submit(world, Input(), 0).HasError());
        const auto other = Admit(coordinator, world, Input(Caller(2)));
        REQUIRE(coordinator.Dispatch(1) == 2);
        REQUIRE(coordinator.Dispatch(1) == 0);
        jobs.Shutdown(ShutdownPolicy::Drain);
        const NavigationPathCaller callers[]{Caller(), Caller(2)};
        // Stable admission prefix holds caller 2 behind earlier pending requests, even though its query finished.
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        REQUIRE_FALSE(coordinator.Take(other));
        for (std::uint32_t index = 1; index < 4; ++index)
            REQUIRE(coordinator.Cancel({World(), {index, 1}}));
        REQUIRE(coordinator.Commit(Current(callers)) == 4);
        auto result = coordinator.Take(other);
        REQUIRE(result);
        REQUIRE(result->job != 0);
        REQUIRE(ErrorIs(*result, NavigationErrors::NoNavigationData));
    }

    TEST_CASE("Priority aging and deadlines are bounded under Foundation pressure", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0, .maxQueuedJobs = 1}};
        auto blocked = jobs.SubmitResult({}, [](const CancellationToken &) {
            return Result<void>::Success();
        });
        REQUIRE(blocked.HasValue());
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world);
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
        auto input = Input();
        input.deadlineTick = 2;
        const auto handle = Admit(coordinator, world, input);
        REQUIRE(coordinator.Dispatch(1) == 0);
        REQUIRE(coordinator.Dispatch(2) == 0);
        const NavigationPathCaller callers[]{Caller()};
        REQUIRE(coordinator.Commit(Current(callers, 2)) == 0);
        REQUIRE(coordinator.Commit(Current(callers, 3)) == 1);
        auto result = coordinator.Take(handle);
        REQUIRE(result);
        REQUIRE(ErrorIs(*result, NavigationErrors::CapacityExceeded));
        REQUIRE(result->job == 0);
        REQUIRE(coordinator.IsDrained());
    }

    TEST_CASE("Owner fences reject changed revisions replacement callers and unloaded worlds", "[unit][navigation][coordinator]") {
        for (int fence = 0; fence < 7; ++fence) {
            JobSystem jobs{{.workerCount = 0}};
            auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
            Activate(world);
            auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
            const auto handle = Admit(coordinator, world);
            REQUIRE(coordinator.Dispatch(1) == 1);
            jobs.Shutdown(ShutdownPolicy::Drain);
            const NavigationPathCaller callers[]{fence == 5 ? Caller(1, 2) : Caller()};
            auto current = Current(callers);
            if (fence == 0)
                ++current.source.filterRevision;
            if (fence == 1)
                ++current.source.profileRevision;
            if (fence == 2)
                ++current.source.originRevision;
            if (fence == 3)
                ++current.source.obstacleRevision;
            if (fence == 4)
                current.source.snapshot = NavigationSnapshotToken::Create(2).Value();
            if (fence == 6) {
                REQUIRE(world.Unload(World()).HasValue());
                current.activation = {};
                current.source = {};
            }
            REQUIRE(coordinator.Commit(current) == 1);
            auto result = coordinator.Take(handle);
            REQUIRE(result);
            REQUIRE(ErrorIs(*result, fence == 5   ? NavigationErrors::InvalidHandle
                                     : fence == 6 ? NavigationErrors::InvalidWorld
                                                  : NavigationErrors::StaleSnapshot));
        }
    }

    TEST_CASE("Queued scheduler cancellation and exception publish each request terminally", "[unit][navigation][coordinator]") {
        for (bool fail : {false, true}) {
            JobSystem jobs{{.workerCount = 0}};
            auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
            Activate(world, std::make_unique<ControlledBackend>(nullptr, fail));
            auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
            const auto first = Admit(coordinator, world);
            auto secondInput = Input(Caller(2));
            secondInput.request.start.x = 2.0F;
            const auto second = Admit(coordinator, world, secondInput);
            REQUIRE(coordinator.Dispatch(1) == 2);
            jobs.Shutdown(fail ? ShutdownPolicy::Drain : ShutdownPolicy::Cancel);
            const NavigationPathCaller callers[]{Caller(), Caller(2)};
            REQUIRE(coordinator.Commit(Current(callers)) == 2);
            const auto firstResult = coordinator.Take(first);
            const auto secondResult = coordinator.Take(second);
            REQUIRE(firstResult);
            REQUIRE(secondResult);
            REQUIRE(firstResult->result.HasValue() == fail);
            REQUIRE(secondResult->result.HasError());
            REQUIRE_FALSE(coordinator.Take(first));
            REQUIRE(coordinator.IsDrained());
        }
    }

    TEST_CASE("Running cancellation holds slot and provider lifetime until worker drain", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 1}};
        auto gate = std::make_shared<Gate>();
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world, std::make_unique<ControlledBackend>(gate));
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
        const auto handle = Admit(coordinator, world);
        REQUIRE(coordinator.Dispatch(1) == 1);
        const bool entered = gate->AwaitEntry();
        REQUIRE(coordinator.Cancel(handle));
        const NavigationPathCaller callers[]{Caller()};
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        auto cancelled = coordinator.Take(handle);
        REQUIRE(cancelled);
        REQUIRE(ErrorIs(*cancelled, NavigationErrors::QueryCancelled));
        REQUIRE_FALSE(coordinator.IsDrained());
        REQUIRE(world.Unload(World()).HasValue());
        REQUIRE(world.RetiredCount() == 1);
        gate->Release();
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE_FALSE(coordinator.Take(handle));
        REQUIRE(coordinator.IsDrained());
        REQUIRE(world.CollectRetired() == NavigationWorldLifecycleState::Empty);
        REQUIRE(entered);
    }

    TEST_CASE("Out of order workers cannot grant an earlier publication slot", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 2}};
        auto gate = std::make_shared<Gate>();
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world, std::make_unique<ControlledBackend>(gate));
        auto limits = Limits();
        limits.requestsPerJob = 1;
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, limits)).Value();
        const auto first = Admit(coordinator, world);
        auto input = Input(Caller(2));
        input.request.start.x = 2.0F;
        const auto second = Admit(coordinator, world, input);
        REQUIRE(coordinator.Dispatch(1) == 2);
        const bool entered = gate->AwaitEntry();
        const bool otherFinished = gate->AwaitOther();
        const NavigationPathCaller callers[]{Caller(), Caller(2)};
        REQUIRE(coordinator.Commit(Current(callers)) == 0);
        REQUIRE_FALSE(coordinator.Take(second));
        gate->Release();
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(coordinator.Commit(Current(callers)) == 2);
        auto a = coordinator.Take(first);
        auto b = coordinator.Take(second);
        REQUIRE(a);
        REQUIRE(b);
        REQUIRE(a->acceptedSequence < b->acceptedSequence);
        REQUIRE(a->result.HasValue());
        REQUIRE(b->result.HasValue());
        REQUIRE(entered);
        REQUIRE(otherFinished);
    }

    TEST_CASE("Priority selection ages old callers without bypassing admission publication", "[unit][navigation][coordinator]") {
        for (const std::uint64_t aging : {3U, 30U}) {
            JobSystem jobs{{.workerCount = 0}};
            auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
            Activate(world);
            auto limits = Limits();
            limits.requestsPerCaller = 1;
            limits.priorityAgingTicks = aging;
            auto coordinator = std::move(NavigationCoordinator::Create(jobs, limits)).Value();
            auto oldInput = Input();
            oldInput.priority = JobPriority::Background;
            const auto old = Admit(coordinator, world, oldInput);
            auto freshInput = Input();
            freshInput.priority = JobPriority::Interactive;
            freshInput.targetTick = 5;
            auto admitted = coordinator.Submit(world, freshInput, 5);
            REQUIRE(admitted.HasValue());
            const auto fresh = admitted.Value();
            REQUIRE(coordinator.Dispatch(5) == 1);
            jobs.Shutdown(ShutdownPolicy::Drain);
            REQUIRE(coordinator.Cancel(old));
            REQUIRE(coordinator.Cancel(fresh));
            const NavigationPathCaller callers[]{Caller()};
            REQUIRE(coordinator.Commit(Current(callers, 5)) == 2);
            const auto oldResult = coordinator.Take(old);
            const auto freshResult = coordinator.Take(fresh);
            REQUIRE(oldResult);
            REQUIRE(freshResult);
            REQUIRE((oldResult->job != 0) == (aging == 3));
            REQUIRE((freshResult->job != 0) == (aging == 30));
        }
    }

    TEST_CASE("Node reservations and provider concurrency remain hard bounds", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world);
        auto limits = Limits();
        limits.nodeExpansionsPerTick = 128;
        limits.nodeExpansionsPerCaller = 64;
        limits.requestsPerJob = 1;
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, limits)).Value();
        const auto first = Admit(coordinator, world);
        const auto second = Admit(coordinator, world, Input(Caller(2)));
        // Provider owns only one query scratch lease, despite two configured Foundation partition slots.
        REQUIRE(coordinator.Dispatch(1) == 1);
        REQUIRE(coordinator.Dispatch(1) == 0);
        jobs.Shutdown(ShutdownPolicy::Drain);
        const NavigationPathCaller callers[]{Caller(), Caller(2)};
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        REQUIRE(coordinator.Take(first));
        REQUIRE(coordinator.Cancel(second));
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        REQUIRE(coordinator.Take(second));
        REQUIRE(coordinator.IsDrained());
    }

    TEST_CASE("Cancellation after publication cannot change retained results and handles never alias reuse",
              "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world);
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
        const auto old = Admit(coordinator, world);
        REQUIRE(coordinator.Cancel(old));
        const NavigationPathCaller callers[]{Caller()};
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        REQUIRE_FALSE(coordinator.Cancel(old));
        REQUIRE(coordinator.Take(old));
        auto next = coordinator.Submit(world, Input(), 1);
        REQUIRE(next.HasValue());
        REQUIRE(next.Value().slot.index == old.slot.index);
        REQUIRE(next.Value().slot.generation > old.slot.generation);
        REQUIRE_FALSE(coordinator.Cancel(old));
        REQUIRE_FALSE(coordinator.Take(old));
        REQUIRE(coordinator.Cancel(next.Value()));
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        REQUIRE(coordinator.Take(next.Value()));
    }

    TEST_CASE("Partial provider results preserve exact corridor status without fabricated coverage", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world, std::make_unique<ControlledBackend>(nullptr, false, true));
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
        auto input = Input();
        input.request.coveragePolicy = NavigationPathCoveragePolicy::AllowPartial;
        const auto handle = Admit(coordinator, world, input);
        REQUIRE(coordinator.Dispatch(1) == 1);
        jobs.Shutdown(ShutdownPolicy::Drain);
        const NavigationPathCaller callers[]{Caller()};
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        const auto result = coordinator.Take(handle);
        REQUIRE(result);
        REQUIRE(result->result.HasValue());
        REQUIRE(result->result.Value().status == NavigationPathStatus::Partial);
        REQUIRE(result->result.Value().stopReason == NavigationPathStopReason::NodeBudgetExceeded);
        REQUIRE(result->result.Value().points.size() == 2);
    }

    TEST_CASE("Pause closes admission while existing work survives and shutdown publishes once", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world);
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
        const auto handle = Admit(coordinator, world);
        REQUIRE(world.Pause(World()).HasValue());
        REQUIRE(coordinator.Submit(world, Input(Caller(2)), 0).HasError());
        REQUIRE(coordinator.Dispatch(1) == 1);
        coordinator.BeginShutdown();
        REQUIRE(coordinator.Submit(world, Input(), 1).HasError());
        const NavigationPathCaller callers[]{Caller()};
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        auto result = coordinator.Take(handle);
        REQUIRE(result);
        REQUIRE(ErrorIs(*result, NavigationErrors::QueryCancelled));
        REQUIRE_FALSE(coordinator.Take(handle));
        REQUIRE(coordinator.IsDrained());
    }

    TEST_CASE("Coordinator destruction revokes queued work without retaining borrowed lifecycle state", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto destructions = std::make_shared<std::atomic<std::uint32_t>>();
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world, MakeObservedNavigationBackend(destructions));
        {
            auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
            static_cast<void>(Admit(coordinator, world));
            REQUIRE(coordinator.Dispatch(1) == 1);
        }
        REQUIRE(world.Unload(World()).HasValue());
        static_cast<void>(world.CollectRetired());
        REQUIRE(destructions->load() == 1);
    }

    TEST_CASE("Caller cancellation ancestry wins after provider completion before publication", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world, std::make_unique<ControlledBackend>());
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
        CancellationSource parent;
        auto input = Input();
        input.cancellation = parent.Token();
        const auto handle = Admit(coordinator, world, input);
        REQUIRE(coordinator.Dispatch(1) == 1);
        jobs.Shutdown(ShutdownPolicy::Drain);
        parent.RequestCancellation();
        const NavigationPathCaller callers[]{Caller()};
        REQUIRE(coordinator.Commit(Current(callers)) == 1);
        const auto result = coordinator.Take(handle);
        REQUIRE(result);
        REQUIRE(ErrorIs(*result, NavigationErrors::QueryCancelled));
        REQUIRE_FALSE(coordinator.Take(handle));
    }

}  // namespace Horo::Navigation
