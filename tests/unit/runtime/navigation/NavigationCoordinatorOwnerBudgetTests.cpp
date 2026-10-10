#include "navigation/NavigationCoordinatorTestFixtures.h"

namespace Horo::Navigation {
    using namespace CoordinatorTestSupport;

    TEST_CASE("Owner selection quota cannot reset within a tick and preserves rotating caller fairness",
              "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world, std::make_unique<ControlledBackend>());
        auto limits = Limits();
        limits.selectionProbesPerTick = limits.requestSlots;
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, limits)).Value();
        const auto first = Admit(coordinator, world);
        const auto sameCaller = Admit(coordinator, world);
        const auto other = Admit(coordinator, world, Input(Caller(2)));
        REQUIRE(coordinator.Dispatch(1) == 1);
        REQUIRE(coordinator.Dispatch(1) == 0);
        REQUIRE(coordinator.Dispatch(2) == 1);
        REQUIRE(coordinator.Dispatch(2) == 0);
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(coordinator.Cancel(first));
        REQUIRE(coordinator.Cancel(sameCaller));
        REQUIRE(coordinator.Cancel(other));
        const NavigationPathCaller callers[]{Caller(), Caller(2)};
        REQUIRE(coordinator.Commit(Current(callers, 2)) == 3);
        const auto a = coordinator.Take(first);
        const auto b = coordinator.Take(sameCaller);
        const auto c = coordinator.Take(other);
        REQUIRE(a);
        REQUIRE(b);
        REQUIRE(c);
        REQUIRE(a->job != 0);
        REQUIRE(b->job == 0);
        REQUIRE(c->job != 0);
        REQUIRE(coordinator.IsDrained());
    }

    TEST_CASE("Ambiguous or unordered caller authority cannot publish any result", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world);
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
        const auto handle = Admit(coordinator, world);
        REQUIRE(coordinator.Cancel(handle));
        const NavigationPathCaller ambiguous[]{Caller(), Caller(1, 2)};
        const NavigationPathCaller unordered[]{Caller(2), Caller()};
        REQUIRE(coordinator.Commit(Current(ambiguous)) == 0);
        REQUIRE(coordinator.Commit(Current(unordered)) == 0);
        REQUIRE_FALSE(coordinator.Take(handle));
        const NavigationPathCaller valid[]{Caller()};
        REQUIRE(coordinator.Commit(Current(valid)) == 1);
        const auto result = coordinator.Take(handle);
        REQUIRE(result);
        REQUIRE(ErrorIs(*result, NavigationErrors::QueryCancelled));
    }

    TEST_CASE("Selection profiles reject less than one scan and excessive compiled owner work", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 0}};
        auto limits = Limits();
        limits.selectionProbesPerTick = limits.requestSlots - 1;
        REQUIRE(NavigationCoordinator::Create(jobs, limits).HasError());
        limits.selectionProbesPerTick = MaximumNavigationPathSelectionProbes + 1;
        REQUIRE(NavigationCoordinator::Create(jobs, limits).HasError());
        limits.selectionProbesPerTick = limits.requestSlots;
        REQUIRE(NavigationCoordinator::Create(jobs, limits).HasValue());
    }

}  // namespace Horo::Navigation
