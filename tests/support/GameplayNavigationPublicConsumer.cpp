#include "Horo/Gameplay/GameplayNavigationContext.h"

#include <type_traits>
static_assert(std::is_copy_constructible_v<Horo::Gameplay::GameplayNavigationHandle>);
static_assert(!std::is_constructible_v<Horo::Gameplay::GameplayNavigationHandle>);

int main() {
    using namespace Horo::Gameplay;
    auto absent = GameplayNavigationContext::Create({.moduleId = "game.consumer", .queryGranted = true}, nullptr, nullptr, nullptr, {});
    if (absent.HasError())
        return 1;
    auto result = absent.Value()->Submit({});
    return result.HasError() && result.ErrorValue().code.Value() == Horo::Navigation::NavigationErrors::CapabilityUnavailable.code.Value()
               ? 0
               : 2;
}
