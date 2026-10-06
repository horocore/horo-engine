#include "Horo/Gameplay/GameModule.h"
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

int main() {
    return 0;
}
