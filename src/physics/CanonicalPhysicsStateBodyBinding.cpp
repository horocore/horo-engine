#include "CanonicalPhysicsStateBindingInternal.h"

namespace Horo::Physics::Detail {
    namespace {
        struct Resident final {
            BodyHandle handle;
            JPH::BodyID native;
            bool hasPolicyIntent{};
        };

        /** @brief Body and query-fixture ownership form the whole native inventory; no
         * physical/native slot is invented. */
        std::vector<Resident> Inventory(const CanonicalWorld &world) {
            std::vector<Resident> result;
            result.reserve(world.scene.bodies.size() + world.query.fixtures.size());
            for (const auto &body : world.scene.bodies)
                result.push_back({body.handle, body.nativeBody, true});
            for (const auto &fixture : world.query.fixtures)
                result.push_back({fixture.fixture.body, fixture.nativeBody});
            std::ranges::sort(result, {}, &Resident::handle);
            return result;
        }

        /** @brief Physical identity validity comes from the actual owner registry and
         * current native object. */
        Result<CanonicalPhysicsStateBodyBinding> Bind(const CanonicalWorld &world, const PhysicsStateBodyBinding &binding,
                                                      std::span<const Resident> inventory) {
            const auto found = std::ranges::lower_bound(inventory, binding.body, {}, &Resident::handle);
            if (found == inventory.end() || found->handle != binding.body)
                return Result<CanonicalPhysicsStateBodyBinding>::Failure(MakeError(PhysicsErrors::HandleStale));
            const auto *native = world.native.system->GetBodyLockInterfaceNoLock().TryGetBody(found->native);
            if (!native || !native->IsInBroadPhase() || !native->IsRigidBody())
                return Result<CanonicalPhysicsStateBodyBinding>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            return Result<CanonicalPhysicsStateBodyBinding>::Success({binding.id, binding.body, found->native.GetIndexAndSequenceNumber(),
                                                                      native->GetMotionPropertiesUnchecked() != nullptr,
                                                                      found->hasPolicyIntent});
        }
    }  // namespace

    /** @copydoc BindCanonicalStateBodies */
    Result<void> BindCanonicalStateBodies(const CanonicalWorld &world, std::span<const PhysicsStateBodyBinding> bindings,
                                          CanonicalPhysicsStateBindingStorage &storage) {
        const auto inventory = Inventory(world);
        if (inventory.size() != bindings.size() || inventory.size() != world.native.system->GetNumBodies())
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics checkpoint requires every resident body binding."));
        storage.stableBodies.reserve(bindings.size());
        for (const auto &binding : bindings) {
            if (binding.id == 0 || !binding.body.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto resolved = Bind(world, binding, inventory);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            storage.stableBodies.push_back(std::move(resolved).Value());
        }
        std::ranges::sort(storage.stableBodies, {}, &CanonicalPhysicsStateBodyBinding::id);
        storage.nativeBodies = storage.stableBodies;
        std::ranges::sort(storage.nativeBodies, {}, &CanonicalPhysicsStateBodyBinding::nativeId);
        for (std::size_t index = 1; index < bindings.size(); ++index)
            if (storage.stableBodies[index - 1].id == storage.stableBodies[index].id ||
                storage.nativeBodies[index - 1].nativeId == storage.nativeBodies[index].nativeId)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Physics checkpoint body bindings are not unique."));
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
