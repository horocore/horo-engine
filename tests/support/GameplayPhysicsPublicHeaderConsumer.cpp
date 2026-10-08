#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"

#include <type_traits>

static_assert(
    std::is_same_v<decltype(Horo::Gameplay::GameRuntimeContext::physics), std::shared_ptr<const Horo::Gameplay::GameplayPhysicsContext>>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Gameplay::GameplayPhysicsContext &>().Acquire("game.tests", 1, 1)),
                             Horo::Result<Horo::Physics::PhysicsQueryEventCapability>>);

static_assert(std::is_same_v<decltype(std::declval<const Horo::Gameplay::GameplayPhysicsContext &>().AcquireCharacterClearance(
                                 "game.tests", 1, Horo::Character::CharacterPhysicsQueryExpectations{})),
                             Horo::Result<Horo::Physics::CharacterClearanceQuery>>);

int main() {
    return 0;
}
