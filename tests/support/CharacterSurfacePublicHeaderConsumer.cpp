#include "Horo/Physics/CharacterControllerContracts.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterGroundSurfaceFact>);
static_assert(std::is_same_v<decltype(Horo::Character::CharacterMovementResult{}.gravityVelocityMetersPerSecond), Horo::Math::Vec3>);
static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterCollisionSelectors>);
static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterSurfaceContact>);

int main() {
    Horo::Character::CharacterLocomotionSnapshot snapshot;
    Horo::Character::CharacterControllerDescriptor descriptor;
    descriptor.steepSlopePolicy = Horo::Character::CharacterSteepSlopePolicy::Slide;
    descriptor.preserveHorizontalSpeedOnSlopes = true;
    return Horo::Character::BuildCharacterGroundSurfaceFact(snapshot, descriptor).HasError() ? 0 : 1;
}
