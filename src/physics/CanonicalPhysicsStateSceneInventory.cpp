#include "CanonicalPhysicsStateSceneInventory.h"

#include "CanonicalPhysicsRuntimeInternal.h"
#include "Horo/Physics/PhysicsErrors.h"

namespace Horo::Physics::Detail {
    namespace {
        bool Joined(const CanonicalWorld *world) noexcept {
            return world && world->native.system && world->pendingBodyBatch.expired() && !world->contactRoute.load();
        }
    }  // namespace

    /** @copydoc InspectCanonicalStateSceneInventory */
    Result<CanonicalStateSceneCounts> InspectCanonicalStateSceneInventory(CanonicalWorldHandle handle) {
        const auto *world = static_cast<const CanonicalWorld *>(handle.value);
        if (!Joined(world))
            return Result<CanonicalStateSceneCounts>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Result<CanonicalStateSceneCounts>::Success(
            {world->scene.bodies.size(), world->scene.shapes.size(), world->scene.constraints.size(), world->query.fixtures.size()});
    }

    /** @copydoc CopyCanonicalStateSceneInventory */
    Result<void> CopyCanonicalStateSceneInventory(CanonicalWorldHandle handle, CanonicalStateSceneStorage storage) {
        auto counts = InspectCanonicalStateSceneInventory(handle);
        if (counts.HasError())
            return Result<void>::Failure(counts.ErrorValue());
        if (storage.bodies.size() != counts.Value().bodies || storage.shapes.size() != counts.Value().shapes ||
            storage.constraints.size() != counts.Value().constraints)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        const auto &world = *static_cast<const CanonicalWorld *>(handle.value);
        for (std::size_t i = 0; i < storage.bodies.size(); ++i)
            storage.bodies[i] = {world.scene.bodies[i].handle, world.scene.bodies[i].policy.shape, world.scene.bodies[i].sceneEntity};
        for (std::size_t i = 0; i < storage.shapes.size(); ++i)
            storage.shapes[i] = world.scene.shapes[i].handle;
        for (std::size_t i = 0; i < storage.constraints.size(); ++i)
            storage.constraints[i] = world.scene.constraints[i].handle;
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
