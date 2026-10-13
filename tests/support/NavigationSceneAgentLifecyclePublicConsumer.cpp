#include "Horo/Navigation/NavigationSceneAgentLifecycle.h"

#include <type_traits>

static_assert(std::is_final_v<Horo::Navigation::NavigationSceneAgentLifecycle>);
static_assert(!std::is_copy_constructible_v<Horo::Navigation::NavigationSceneAgentLifecycle>);
static_assert(!std::is_default_constructible_v<Horo::Navigation::NavigationSceneAgentLifecycle>);
static_assert(
    std::is_same_v<decltype(&Horo::Navigation::NavigationSceneAgentLifecycle::CancelVelocity),
                   bool (Horo::Navigation::NavigationSceneAgentLifecycle::*)(const Horo::Navigation::NavigationVelocityWork &) noexcept>);

int main() {
    Horo::JobSystem jobs{{.workerCount = 0}};
    auto created = Horo::Navigation::NavigationCoordinator::Create(jobs, {});
    if (created.HasError())
        return 1;
    auto coordinator = std::move(created).Value();
    coordinator.CollectRetiredAtSafePoint();
    if (coordinator.IsRetained({}))
        return 3;
    return coordinator.Abandon({}) ? 2 : 0;
}
