#include "Horo/AI/AISceneActivation.h"

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<decltype(std::declval<Horo::AI::AiSceneRuntime &>().MakeStructuralParticipant(
                                 std::declval<std::span<const Horo::AI::AiControllerDescriptor>>())),
                             std::unique_ptr<Horo::Runtime::SceneStructuralParticipant>>);

int main() {
    return 0;
}
