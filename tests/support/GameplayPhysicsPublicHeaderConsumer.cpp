#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameplayCharacterContext.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Physics/PhysicsContinuousCollision.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <type_traits>

static_assert(
    std::is_same_v<decltype(Horo::Gameplay::GameRuntimeContext::physics), std::shared_ptr<const Horo::Gameplay::GameplayPhysicsContext>>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Gameplay::GameplayPhysicsContext &>().Acquire("game.tests", 1, 1)),
                             Horo::Result<Horo::Physics::PhysicsQueryEventCapability>>);

static_assert(std::is_same_v<decltype(std::declval<const Horo::Physics::PhysicsWorld &>().ReadContinuousCollision()),
                             Horo::Result<Horo::Physics::PhysicsContinuousCollisionObservation>>);
static_assert(
    std::is_same_v<decltype(Horo::Physics::PhysicsBodyDescriptor{}.continuousCollision), Horo::Physics::PhysicsBodyContinuousCollision>);

static_assert(std::is_same_v<decltype(Horo::Physics::PhysicsWorldSimulationBinding{}.schema),
                             std::shared_ptr<const Horo::Physics::NormalizedCollisionSchema>>);
static_assert(std::is_same_v<decltype(Horo::Physics::PhysicsSceneBodyDescriptor{}.collision),
                             std::optional<Horo::Physics::PhysicsSceneCollisionBinding>>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Gameplay::GameplayPhysicsContext &>().AcquireCharacterClearance(
                                 "game.tests", 1, Horo::Character::CharacterPhysicsQueryExpectations{})),
                             Horo::Result<Horo::Physics::CharacterClearanceQuery>>);

template <class Context>
concept AcceptsForgedCharacterContext = requires(Horo::Gameplay::GameplayCharacterBinding binding,
                                                 Horo::Character::CharacterCapability client) { Context{{}, binding, client}; };
static_assert(!AcceptsForgedCharacterContext<Horo::Gameplay::GameplayCharacterContext>);
static_assert(std::is_same_v<decltype(Horo::Gameplay::GameRuntimeContext::character),
                             std::shared_ptr<const Horo::Gameplay::GameplayCharacterContext>>);
static_assert(Horo::Gameplay::GameplaySdkBoundaryVersion == 20);

int main() {
    const auto context = Horo::Gameplay::GameplayCharacterContext::Withheld({}, Horo::Gameplay::GameplayCharacterDenial::Unavailable);
    const auto result = context->Acquire("game.tests", 1, 1);
    return result.HasError() && result.ErrorValue().code.Value() == Horo::Gameplay::GameplayErrors::CharacterUnavailable.code.Value() ? 0
                                                                                                                                      : 1;
}
