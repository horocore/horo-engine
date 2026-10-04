#include "Horo/Physics/PhysicsSceneActivation.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <memory>
#include <type_traits>
#include <utility>

using Horo::Physics::PhysicsSceneBodyDescriptor;
using Horo::Physics::PhysicsSceneBodyPreparation;
using Horo::Physics::PhysicsWorld;

static_assert(!std::is_default_constructible_v<PhysicsSceneBodyPreparation>);
static_assert(!std::is_copy_constructible_v<PhysicsSceneBodyPreparation>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Physics::PhysicsSceneActivationCandidate &>().FindRuntimeBody(
                                 std::declval<Horo::Runtime::EntityRef>(), std::declval<Horo::Runtime::PhysicsBodySlotId>())),
                             std::optional<Horo::Physics::BodyHandle>>);
static_assert(std::is_same_v<decltype(std::declval<PhysicsSceneBodyPreparation &>().PrepareRetirement(
                                 std::declval<std::span<const Horo::Physics::BodyHandle>>(),
                                 std::declval<std::span<const Horo::Physics::ShapeHandle>>(),
                                 std::declval<std::span<const Horo::Physics::ConstraintHandle>>())),
                             Horo::Result<void>>);
static_assert(std::is_same_v<decltype(std::declval<const PhysicsWorld &>().PrepareSceneGroup(
                                 std::declval<std::span<const Horo::Physics::PhysicsSceneGroupShape>>(),
                                 std::declval<std::span<const Horo::Physics::PhysicsSceneGroupBody>>())),
                             Horo::Result<std::unique_ptr<PhysicsSceneBodyPreparation>>>);
static_assert(std::is_same_v<decltype(std::declval<const PhysicsWorld &>().PrepareSceneBodies(
                                 std::declval<std::span<const PhysicsSceneBodyDescriptor>>())),
                             Horo::Result<std::unique_ptr<PhysicsSceneBodyPreparation>>>);

int main() {
    return 0;
}
