#include "Horo/Navigation/Backends/DeterministicCrowdProvider.h"
#include "Horo/Navigation/NavigationCoordinator.h"
#include "Horo/Navigation/NavigationCrowdBatch.h"
#include "Horo/Navigation/NavigationCrowdOverload.h"
#include "Horo/Navigation/NavigationCrowdRelevance.h"
#include "Horo/Navigation/NavigationMovement.h"
#include "Horo/Navigation/NavigationVelocityPipeline.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Navigation::NavigationCoordinator>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Navigation::NavigationCoordinator>);
static_assert(std::is_same_v<decltype(Horo::Navigation::NavigationPathCompletion::result), Horo::Result<Horo::Navigation::NavigationPath>>);
static_assert(
    std::is_same_v<decltype(Horo::Navigation::NavigationWorldReadLease{}.Revocation()), Horo::Navigation::NavigationWorldRevocation>);

static_assert(!std::is_copy_constructible_v<Horo::Navigation::NavigationMovementController>);
static_assert(!std::is_copy_constructible_v<Horo::Navigation::NavigationVelocityPipeline>);
static_assert(std::is_same_v<decltype(Horo::Navigation::NavigationVelocityFeedback::achievedDisplacement), Horo::Math::Vec3>);
static_assert(Horo::Navigation::NavigationCrowdProfileFacts{}.densitySaturationNeighbors == 0);
static_assert(Horo::Navigation::NavigationCrowdAgentFact{}.densitySpeedScale == 1.0F);
static_assert(std::is_base_of_v<Horo::Navigation::INavigationCrowdBackend, Horo::Navigation::INavigationQualifiedCrowdBackend>);
static_assert(Horo::Navigation::NavigationCrowdAdmission{}.tier == Horo::Navigation::NavigationCrowdTier::Full);
static_assert(Horo::Navigation::NavigationCrowdCapacity{}.agents == 0);
static_assert(Horo::Navigation::NavigationVelocityPublication{}.recovery == Horo::Navigation::NavigationCrowdRecovery::Clear);
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

template <typename T>
concept PublicVelocityConstructionKey = requires { typename T::ConstructionKey; };
template <typename T>
concept DirectVelocityConstruction =
    requires(Horo::Navigation::NavigationMovementController &movement, Horo::Navigation::NavigationAgentSceneBinding binding,
             Horo::Navigation::CrowdAgentHandle agent,
             Horo::Navigation::NavigationVelocityPolicy policy) { T{{}, movement, binding, agent, std::uint64_t{1}, policy}; };
static_assert(!PublicVelocityConstructionKey<Horo::Navigation::NavigationVelocityPipeline>);
static_assert(!DirectVelocityConstruction<Horo::Navigation::NavigationVelocityPipeline>);
static_assert(std::is_final_v<Horo::Navigation::NavigationVelocityPipeline>);
static_assert(!std::is_default_constructible_v<Horo::Navigation::NavigationVelocityPipeline>);
static_assert(!std::is_constructible_v<Horo::Navigation::NavigationVelocityPipeline, Horo::Navigation::NavigationMovementController &,
                                       Horo::Navigation::NavigationAgentSceneBinding, Horo::Navigation::CrowdAgentHandle, std::uint64_t,
                                       Horo::Navigation::NavigationVelocityPolicy>);

int main() {
    Horo::JobSystem jobs{{.workerCount = 0}};
    auto coordinator = Horo::Navigation::NavigationCoordinator::Create(jobs, {});
    if (coordinator.HasError())
        return 1;
    auto owner = std::move(coordinator).Value();
    owner.BeginShutdown();
    return owner.IsDrained() ? 0 : 2;
}
