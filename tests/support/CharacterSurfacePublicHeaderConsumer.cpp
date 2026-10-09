#include "Horo/Physics/CharacterClearanceQuery.h"
#include "Horo/Physics/CharacterControllerContracts.h"
#include "Horo/Physics/CharacterDebugSnapshot.h"
#include "Horo/Physics/CharacterWorld.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterGroundSurfaceFact>);
static_assert(std::is_same_v<decltype(Horo::Character::CharacterMovementResult{}.gravityVelocityMetersPerSecond), Horo::Math::Vec3>);
static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterCollisionSelectors>);
static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterSurfaceContact>);
static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterPlatformAttachment>);
static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterPlatformBodyEvidence>);
static_assert(std::is_trivially_copyable_v<Horo::Character::CharacterDebugProbe>);
static_assert(std::is_copy_constructible_v<Horo::Character::CharacterDebugSnapshot>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Character::CharacterWorld &>().CaptureDebugSnapshot(
                                 std::declval<const Horo::Character::CharacterDebugCaptureRequest &>())),
                             Horo::Character::CharacterDebugCapture>);

static_assert(std::is_same_v<decltype(std::declval<Horo::Physics::CharacterClearanceQuery &>().Context()),
                             Horo::Character::CharacterPhysicsQueryContext>);
static_assert(std::is_trivially_copyable_v<Horo::Physics::PhysicsCapsuleOverlapQuery>);
static_assert(std::is_same_v<decltype(std::declval<Horo::Character::CharacterWorld &>().RefreshPhysicsSnapshot(
                                 std::declval<Horo::Physics::PhysicsWorldId>(), std::uint64_t{})),
                             Horo::Result<void>>);

int main() {
    Horo::Character::CharacterLocomotionSnapshot snapshot;
    Horo::Character::CharacterControllerDescriptor descriptor;
    descriptor.steepSlopePolicy = Horo::Character::CharacterSteepSlopePolicy::Slide;
    descriptor.preserveHorizontalSpeedOnSlopes = true;
    descriptor.jumpSpeedMetersPerSecond = 5.0F;
    snapshot.movement.groundTransition = Horo::Character::CharacterGroundTransition::Landed;
    snapshot.movement.jumpApplied = false;
    return Horo::Character::BuildCharacterGroundSurfaceFact(snapshot, descriptor).HasError() ? 0 : 1;
}
