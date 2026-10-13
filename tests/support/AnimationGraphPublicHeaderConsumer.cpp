#include "Horo/Animation/AnimationGraph.h"
#include "Horo/Animation/AnimationGraphInstance.h"
#include "Horo/Animation/AnimationGraphSource.h"
#include "Horo/Animation/AnimationParameterCurve.h"

#include <type_traits>
static_assert(!std::is_convertible_v<Horo::Animation::GraphNodeId, Horo::Animation::GraphPinId>);
static_assert(!std::is_convertible_v<Horo::Animation::GraphStateId, Horo::Animation::GraphTransitionId>);
static_assert(std::is_trivially_copyable_v<Horo::Animation::GraphStateEvent>);
static_assert(Horo::Animation::CurrentAnimationGraphSchemaVersion == 4);
static_assert(Horo::Animation::GraphBlendNode{}.mode == Horo::Animation::GraphBlendMode::Normalized);
static_assert(Horo::Animation::GraphBlendNode{}.missingJoint == Horo::Animation::GraphMissingJointPolicy::SkeletonReference);

int main() {
    const auto unavailable = Horo::Animation::AnimationGraphInstance::Create({}, {});
    const auto failed = Horo::Animation::AnimationGraphProgram::Compile({});
    const auto malformedSource = Horo::Animation::DeserializeAnimationGraphSource("{}", {});
    const auto malformedCurve = Horo::Animation::AnimationParameterCurve::Create({});
    Horo::Animation::GraphNode stateNode;
    stateNode.payload = Horo::Animation::GraphStateMachineNode{};
    return std::holds_alternative<Horo::Animation::GraphStateMachineNode>(stateNode.payload) && malformedCurve.HasError() &&
                   unavailable.HasError() && malformedSource.HasError() && failed.HasError() &&
                   failed.ErrorValue().code.Value() == Horo::Animation::AnimationErrors::GraphMalformed.code.Value()
               ? 0
               : 1;
}
