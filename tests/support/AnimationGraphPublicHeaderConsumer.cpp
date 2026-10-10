#include "Horo/Animation/AnimationGraph.h"
#include "Horo/Animation/AnimationGraphSource.h"

#include <type_traits>
static_assert(!std::is_convertible_v<Horo::Animation::GraphNodeId, Horo::Animation::GraphPinId>);

int main() {
    const auto failed = Horo::Animation::AnimationGraphProgram::Compile({});
    const auto malformedSource = Horo::Animation::DeserializeAnimationGraphSource("{}", {});
    return malformedSource.HasError() && failed.HasError() &&
                   failed.ErrorValue().code.Value() == Horo::Animation::AnimationErrors::GraphMalformed.code.Value()
               ? 0
               : 1;
}
