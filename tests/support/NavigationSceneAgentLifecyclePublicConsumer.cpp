#include "Horo/Navigation/NavigationSceneAgentLifecycle.h"

#include <type_traits>

static_assert(std::is_final_v<Horo::Navigation::NavigationSceneAgentLifecycle>);
static_assert(!std::is_copy_constructible_v<Horo::Navigation::NavigationSceneAgentLifecycle>);
static_assert(!std::is_default_constructible_v<Horo::Navigation::NavigationSceneAgentLifecycle>);
static_assert(!std::is_constructible_v<Horo::Navigation::NavigationSceneAgentLifecycle, Horo::Navigation::NavigationAgentRegistry &,
                                       Horo::Runtime::EntityRef, std::unique_ptr<Horo::Navigation::NavigationMovementController>,
                                       std::unique_ptr<Horo::Navigation::NavigationVelocityPipeline>, std::uint64_t>);
static_assert(std::is_same_v<decltype(&Horo::Navigation::NavigationSceneAgentLifecycle::Create),
                             Horo::Result<std::unique_ptr<Horo::Navigation::NavigationSceneAgentLifecycle>> (*)(
                                 Horo::Navigation::NavigationAgentRegistry &, Horo::Navigation::NavigationWorldLifecycle &,
                                 Horo::Navigation::NavigationCoordinator &, Horo::Runtime::RuntimeSceneView,
                                 const Horo::Navigation::NavigationSceneAgentLifecycleDescriptor &)>);
static_assert(
    std::is_same_v<decltype(&Horo::Navigation::NavigationSceneAgentLifecycle::CancelVelocity),
                   bool (Horo::Navigation::NavigationSceneAgentLifecycle::*)(const Horo::Navigation::NavigationVelocityWork &) noexcept>);

int main() {
    const Horo::Navigation::NavigationSceneAgentLifecycleDescriptor descriptor;
    static_cast<void>(descriptor);
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
