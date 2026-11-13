#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/DecisionGraphInstance.h"

#include <type_traits>

namespace Horo::AI::PublicContract {
    static_assert(!std::is_default_constructible_v<AiDecisionTaskReservation>);
    static_assert(!std::is_copy_constructible_v<AiDecisionTaskReservation>);
    static_assert(std::is_nothrow_move_constructible_v<AiDecisionTaskReservation>);
    static_assert(std::is_same_v<decltype(std::declval<const AiSceneRuntime &>().ReserveDecisionTaskSlotsAtSafePoint(AgentHandle{}, 1)),
                                 Result<AiDecisionTaskReservation>>);
    static_assert(std::is_same_v<decltype(std::declval<const AiSceneRuntime &>().CaptureDecisionSnapshotAtSafePoint(AgentHandle{})),
                                 Result<BlackboardSnapshot>>);
    using SceneGraphFactory = Result<std::unique_ptr<DecisionGraphInstance>> (*)(std::shared_ptr<const CookedDecisionPlan>,
                                                                                 std::shared_ptr<const DecisionNodeGeneration>,
                                                                                 AiDecisionTaskReservation, AiTaskOperationContext);
    static_assert(std::is_same_v<decltype(static_cast<SceneGraphFactory>(&DecisionGraphInstance::Create)), SceneGraphFactory>);
}  // namespace Horo::AI::PublicContract
