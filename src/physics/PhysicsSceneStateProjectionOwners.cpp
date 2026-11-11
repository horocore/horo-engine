#include "PhysicsSceneStateProjectionInternal.h"

#include <algorithm>

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsSceneStateProjection::ReadInventory */
    Result<void> PhysicsSceneStateProjection::ReadInventory(const PhysicsWorld &world, const CanonicalStateSceneCounts &counts) {
        nativeBodies_.resize(counts.bodies);
        nativeShapes_.resize(counts.shapes);
        nativeConstraints_.resize(counts.constraints);
        rows_.reserve(counts.bodies + counts.shapes + counts.constraints);
        idByNative_.resize(counts.bodies + counts.shapes + counts.constraints);
        bindings_.bodies.reserve(counts.bodies);
        bindings_.shapes.reserve(counts.shapes);
        bindings_.constraints.reserve(counts.constraints);
        return PhysicsWorldStateRestartAccess::CopyInventory(world, {nativeBodies_, nativeShapes_, nativeConstraints_});
    }

    /** @copydoc PhysicsSceneStateProjection::AddOwners */
    Result<void> PhysicsSceneStateProjection::AddOwners(const PhysicsStructuralState &state,
                                                        const Runtime::PersistentEntityIdentityMap &identities) {
        for (const auto &binding : state.bodies) {
            auto key = ResolveSceneStateKey(identities, binding.entity, SceneStateKeyRole::Body, binding.slot.value);
            if (key.HasError())
                return Result<void>::Failure(key.ErrorValue());
            rows_.push_back({key.Value(), binding.handle, {}, {}, {}});
        }
        for (const auto &binding : state.shapes) {
            auto key = ResolveSceneStateKey(identities, binding.entity, SceneStateKeyRole::Collider, binding.slot.value);
            if (key.HasError())
                return Result<void>::Failure(key.ErrorValue());
            rows_.push_back({key.Value(), {}, binding.handle, {}, binding.body});
        }
        for (const auto &binding : state.constraints) {
            auto key = ResolveSceneStateKey(identities, binding.entity, SceneStateKeyRole::Constraint, binding.slot.value);
            if (key.HasError())
                return Result<void>::Failure(key.ErrorValue());
            rows_.push_back({key.Value(), {}, {}, binding.handle, {}});
        }
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneStateProjection::IndexInitialOwners */
    void PhysicsSceneStateProjection::IndexInitialOwners() {
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].body.IsValid())
                bindings_.bodies.emplace_back(i + 1, rows_[i].body);
            if (rows_[i].shape.IsValid())
                bindings_.shapes.emplace_back(i + 1, rows_[i].shape);
        }
        // IDs are private row-index scratch until complete coverage and canonical key order succeed.
        std::ranges::sort(bindings_.bodies, {}, &PhysicsStateBodyBinding::body);
        std::ranges::sort(bindings_.shapes, {}, &PhysicsStateShapeBinding::shape);
    }

    /** @copydoc PhysicsSceneStateProjection::AddCompoundRoots */
    Result<void> PhysicsSceneStateProjection::AddCompoundRoots() {
        IndexInitialOwners();
        for (const auto &body : nativeBodies_) {
            if (const auto existing = std::ranges::lower_bound(bindings_.shapes, body.shape, {}, &PhysicsStateShapeBinding::shape);
                existing != bindings_.shapes.end() && existing->shape == body.shape) {
                if (rows_[existing->id - 1].ownerBody != body.handle)
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
                continue;
            }
            const auto owner = std::ranges::lower_bound(bindings_.bodies, body.handle, {}, &PhysicsStateBodyBinding::body);
            if (owner == bindings_.bodies.end() || owner->body != body.handle)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto key = rows_[owner->id - 1].key;
            key.role = SceneStateKeyRole::CompoundBodyRoot;
            rows_.push_back({key, {}, body.shape, {}, body.handle});
        }
        bindings_.bodies.clear();
        bindings_.shapes.clear();
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
