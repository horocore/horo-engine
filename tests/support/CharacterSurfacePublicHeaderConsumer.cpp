#include "Horo/Physics/CharacterClearanceQuery.h"
#include "Horo/Physics/CharacterControllerContracts.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterGroundSurfaceFact>);
static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterSurfaceContact>);

static_assert(std::is_same_v<decltype(std::declval<Horo::Physics::CharacterClearanceQuery &>().Context()),
                             Horo::Character::CharacterPhysicsQueryContext>);
static_assert(std::is_trivially_copyable_v<Horo::Physics::PhysicsCapsuleOverlapQuery>);

int main() {
    Horo::Character::CharacterLocomotionSnapshot snapshot;
    Horo::Character::CharacterControllerDescriptor descriptor;
    return Horo::Character::BuildCharacterGroundSurfaceFact(snapshot, descriptor).HasError() ? 0 : 1;
}
