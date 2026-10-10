#include "Horo/Navigation/NavigationCoordinator.h"
#include "Horo/Navigation/NavigationMovement.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Navigation::NavigationCoordinator>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Navigation::NavigationCoordinator>);
static_assert(std::is_same_v<decltype(Horo::Navigation::NavigationPathCompletion::result), Horo::Result<Horo::Navigation::NavigationPath>>);

static_assert(!std::is_copy_constructible_v<Horo::Navigation::NavigationMovementController>);
static_assert(std::is_same_v<decltype(Horo::Navigation::NavigationMovementIntent::desiredVelocity), Horo::Math::Vec3>);

template <typename T>
concept DirectMovementConstruction =
    requires(Horo::Navigation::NavigationAgentRegistry &registry, Horo::Navigation::NavigationWorldLifecycle &world,
             Horo::Navigation::NavigationCoordinator &coordinator, const Horo::Navigation::NavigationAgentRecord &agent,
             Horo::Navigation::NavigationAgentSceneBinding binding,
             Horo::Navigation::NavigationPathCaller caller) { T{{}, registry, world, coordinator, agent, binding, caller}; };

static_assert(!DirectMovementConstruction<Horo::Navigation::NavigationMovementController>);
static_assert(!std::is_constructible_v<Horo::Navigation::NavigationMovementController, Horo::Navigation::NavigationAgentRegistry &,
                                       Horo::Navigation::NavigationWorldLifecycle &, Horo::Navigation::NavigationCoordinator &,
                                       const Horo::Navigation::NavigationAgentRecord &, Horo::Navigation::NavigationAgentSceneBinding,
                                       Horo::Navigation::NavigationPathCaller>);
static_assert(std::is_final_v<Horo::Navigation::NavigationMovementController>);

int main() {
    Horo::JobSystem jobs{{.workerCount = 0}};
    auto coordinator = Horo::Navigation::NavigationCoordinator::Create(jobs, {});
    if (coordinator.HasError())
        return 1;
    auto owner = std::move(coordinator).Value();
    owner.BeginShutdown();
    return owner.IsDrained() ? 0 : 2;
}
