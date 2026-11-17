#include "Horo/Physics/PhysicsCapabilities.h"
#include "Horo/Physics/PhysicsStateSnapshot.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <type_traits>

static_assert(Horo::Physics::PhysicsStateSnapshotVersion == 2);
static_assert(Horo::Physics::PhysicsStateSolverSchemaVersion == 1);
static_assert(Horo::Physics::PhysicsCapabilityContractVersion == 2);
static_assert(std::is_copy_constructible_v<Horo::Physics::PhysicsStateSnapshotLease>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Physics::PhysicsStateSnapshotLease>);
static_assert(!std::is_copy_constructible_v<Horo::Physics::PhysicsStateRestoreCandidate>);
static_assert(!std::is_default_constructible_v<Horo::Physics::PhysicsStateRestoreCandidate>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Physics::PhysicsStateRestoreCandidate>);
static_assert(!std::is_copy_constructible_v<Horo::Physics::PhysicsRuntimeRetirement>);

int main() {
    const Horo::Physics::PhysicsStateSnapshotLease empty;
    const Horo::Physics::PhysicsCapabilities metadata;
    return !empty.IsValid() && metadata.contractVersion == 2 ? 0 : 1;
}
