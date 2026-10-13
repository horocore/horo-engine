#include "Horo/Animation/AnimationGraph.h"
#include "Horo/Animation/AnimationGraphInstance.h"
#include "Horo/Animation/AnimationGraphSource.h"
#include "Horo/Animation/AnimationParameterCurve.h"

#include <type_traits>
static_assert(!std::is_convertible_v<Horo::Animation::GraphNodeId, Horo::Animation::GraphPinId>);
static_assert(Horo::Animation::GraphBlendNode{}.mode == Horo::Animation::GraphBlendMode::Normalized);
static_assert(Horo::Animation::GraphBlendNode{}.missingJoint == Horo::Animation::GraphMissingJointPolicy::SkeletonReference);
static_assert(Horo::Animation::CurrentAnimationGraphSchemaVersion == 3);

int main() {
    const auto unavailable = Horo::Animation::AnimationGraphInstance::Create({}, {});
    const auto failed = Horo::Animation::AnimationGraphProgram::Compile({});
    const auto malformedSource = Horo::Animation::DeserializeAnimationGraphSource("{}", {});
    const auto malformedCurve = Horo::Animation::AnimationParameterCurve::Create({});
    return malformedCurve.HasError() && unavailable.HasError() && malformedSource.HasError() && failed.HasError() &&
                   failed.ErrorValue().code.Value() == Horo::Animation::AnimationErrors::GraphMalformed.code.Value()
               ? 0
               : 1;
}
