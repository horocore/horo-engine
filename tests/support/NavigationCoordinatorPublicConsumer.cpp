#include "Horo/Navigation/NavigationCoordinator.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Navigation::NavigationCoordinator>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Navigation::NavigationCoordinator>);
static_assert(std::is_same_v<decltype(Horo::Navigation::NavigationPathCompletion::result), Horo::Result<Horo::Navigation::NavigationPath>>);

int main() {
    Horo::JobSystem jobs{{.workerCount = 0}};
    auto coordinator = Horo::Navigation::NavigationCoordinator::Create(jobs, {});
    if (coordinator.HasError())
        return 1;
    coordinator.Value().BeginShutdown();
    return coordinator.Value().IsDrained() ? 0 : 2;
}
