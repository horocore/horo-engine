#include "PhysicsSceneStateProjectionInternal.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>

namespace Horo::Physics::Detail {
    namespace {
        template <class T> void AddCapacity(std::size_t &bytes, const std::vector<T> &values) noexcept {
            const auto maximum = std::numeric_limits<std::size_t>::max();
            if (values.capacity() > (maximum - bytes) / sizeof(T))
                bytes = maximum;
            else
                bytes += values.capacity() * sizeof(T);
        }
    }  // namespace

    /** @copydoc PhysicsSceneStateProjection::RetainedPayloadBytes */
    std::size_t PhysicsSceneStateProjection::RetainedPayloadBytes() const noexcept {
        auto bytes = sizeof(*this);
        AddCapacity(bytes, rows_);
        AddCapacity(bytes, nativeBodies_);
        AddCapacity(bytes, nativeShapes_);
        AddCapacity(bytes, nativeConstraints_);
        AddCapacity(bytes, bindings_.bodies);
        AddCapacity(bytes, bindings_.shapes);
        AddCapacity(bytes, bindings_.constraints);
        AddCapacity(bytes, routes_);
        AddCapacity(bytes, idByNative_);
        AddCapacity(bytes, owners_);
        return bytes;
    }

    /** @copydoc PhysicsSceneStateProjection::PrepareCoverage */
    void PhysicsSceneStateProjection::PrepareCoverage() {
        for (const auto &row : rows_) {
            if (row.body.IsValid())
                bindings_.bodies.push_back({0, row.body});
            if (row.shape.IsValid())
                bindings_.shapes.push_back({0, row.shape});
            if (row.constraint.IsValid())
                bindings_.constraints.push_back({0, row.constraint});
        }
        std::ranges::sort(bindings_.bodies, {}, &PhysicsStateBodyBinding::body);
        std::ranges::sort(bindings_.shapes, {}, &PhysicsStateShapeBinding::shape);
        std::ranges::sort(bindings_.constraints, {}, &PhysicsStateConstraintBinding::constraint);
        std::ranges::sort(nativeBodies_, {}, &CanonicalStateSceneBody::handle);
        std::ranges::sort(nativeShapes_);
        std::ranges::sort(nativeConstraints_);
    }

    /** @copydoc PhysicsSceneStateProjection::CheckCoverage */
    Result<void> PhysicsSceneStateProjection::CheckCoverage() {
        std::ranges::sort(rows_, {}, &SceneStateKeyBinding::key);
        const auto duplicate = std::ranges::adjacent_find(rows_, {}, &SceneStateKeyBinding::key);
        if (duplicate != rows_.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        PrepareCoverage();
        const auto bodies =
            std::ranges::equal(nativeBodies_, bindings_.bodies, {}, &CanonicalStateSceneBody::handle, &PhysicsStateBodyBinding::body);
        const auto shapes = std::ranges::equal(nativeShapes_, bindings_.shapes, {}, std::identity{}, &PhysicsStateShapeBinding::shape);
        const auto constraints =
            std::ranges::equal(nativeConstraints_, bindings_.constraints, {}, std::identity{}, &PhysicsStateConstraintBinding::constraint);
        if (!bodies || !shapes || !constraints)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        bindings_.bodies.clear();
        bindings_.shapes.clear();
        bindings_.constraints.clear();
        return Result<void>::Success();
    }

    namespace {
        template <class Range, class Handle, class Projection>
        Result<std::size_t> MapId(const Range &native, Handle handle, Projection projection, PhysicsStateObjectId id, std::size_t offset,
                                  std::span<PhysicsStateObjectId> ids) {
            const auto found = std::ranges::lower_bound(native, handle, {}, projection);
            if (found == native.end() || std::invoke(projection, *found) != handle)
                return Result<std::size_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            const auto index = static_cast<std::size_t>(found - native.begin());
            if (offset > ids.size() || index >= ids.size() - offset)
                return Result<std::size_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            ids[offset + index] = id;
            return Result<std::size_t>::Success(index);
        }

        template <class Range, class Handle, class Projection>
        PhysicsStateObjectId FindId(const Range &native, Handle handle, Projection projection, std::size_t offset,
                                    std::span<const PhysicsStateObjectId> ids) noexcept {
            const auto found = std::ranges::lower_bound(native, handle, {}, projection);
            if (found == native.end() || std::invoke(projection, *found) != handle)
                return 0;
            const auto index = static_cast<std::size_t>(found - native.begin());
            if (offset > ids.size() || index >= ids.size() - offset)
                return 0;
            return ids[offset + index];
        }
    }  // namespace

    /** @copydoc PhysicsSceneStateProjection::BindRow */
    Result<void> PhysicsSceneStateProjection::BindRow(PhysicsStateObjectId id, const SceneStateKeyBinding &row) {
        if (row.body.IsValid()) {
            auto index = MapId(nativeBodies_, row.body, &CanonicalStateSceneBody::handle, id, 0, idByNative_);
            if (index.HasError())
                return Result<void>::Failure(index.ErrorValue());
            bindings_.bodies.push_back({id, row.body});
            routes_.push_back({id, nativeBodies_[index.Value()].sceneEntity});
        }
        if (row.shape.IsValid()) {
            auto index = MapId(nativeShapes_, row.shape, std::identity{}, id, nativeBodies_.size(), idByNative_);
            if (index.HasError())
                return Result<void>::Failure(index.ErrorValue());
            bindings_.shapes.push_back({id, row.shape});
        }
        if (row.constraint.IsValid()) {
            auto index =
                MapId(nativeConstraints_, row.constraint, std::identity{}, id, nativeBodies_.size() + nativeShapes_.size(), idByNative_);
            if (index.HasError())
                return Result<void>::Failure(index.ErrorValue());
            bindings_.constraints.push_back({id, row.constraint});
        }
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneStateProjection::BindCanonicalIds */
    Result<void> PhysicsSceneStateProjection::BindCanonicalIds() {
        routes_.reserve(nativeBodies_.size());
        owners_.reserve(rows_.size());
        for (const auto &row : rows_)
            owners_.push_back({row.key.entity.Bytes(), row.key.generation.value, row.key.role, row.key.slot,
                               static_cast<PhysicsStateObjectId>(owners_.size()) + 1});
        for (std::size_t index = 0; index < rows_.size(); ++index)
            if (auto bound = BindRow(static_cast<PhysicsStateObjectId>(index) + 1, rows_[index]); bound.HasError())
                return bound;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneStateProjection::BodyId */
    PhysicsStateObjectId PhysicsSceneStateProjection::BodyId(BodyHandle handle) const noexcept {
        return FindId(nativeBodies_, handle, &CanonicalStateSceneBody::handle, 0, idByNative_);
    }

    /** @copydoc PhysicsSceneStateProjection::ShapeId */
    PhysicsStateObjectId PhysicsSceneStateProjection::ShapeId(ShapeHandle handle) const noexcept {
        return FindId(nativeShapes_, handle, std::identity{}, nativeBodies_.size(), idByNative_);
    }

    /** @copydoc PhysicsSceneStateProjection::ConstraintId */
    PhysicsStateObjectId PhysicsSceneStateProjection::ConstraintId(ConstraintHandle handle) const noexcept {
        return FindId(nativeConstraints_, handle, std::identity{}, nativeBodies_.size() + nativeShapes_.size(), idByNative_);
    }

    /** @copydoc PhysicsSceneStateProjection::PrepareProjection */
    Result<void> PhysicsSceneStateProjection::PrepareProjection(const PhysicsWorld &world, const PhysicsStructuralState &state,
                                                                const Runtime::PersistentEntityIdentityMap &identities,
                                                                const CanonicalStateSceneCounts &counts) {
        if (auto inventory = ReadInventory(world, counts); inventory.HasError())
            return inventory;
        if (auto owners = AddOwners(state, identities); owners.HasError())
            return owners;
        if (auto roots = AddCompoundRoots(); roots.HasError())
            return roots;
        if (auto coverage = CheckCoverage(); coverage.HasError())
            return coverage;
        return BindCanonicalIds();
    }

    /** @copydoc PhysicsSceneStateProjection::Build */
    Result<PhysicsSceneStateProjection> PhysicsSceneStateProjection::Build(const PhysicsWorld &world, const PhysicsStructuralState &state,
                                                                           const Runtime::PersistentEntityIdentityMap &identities,
                                                                           SceneStateProjectionLimits limits) {
        using Outcome = Result<PhysicsSceneStateProjection>;
        auto counts = PhysicsWorldStateRestartAccess::InventoryCounts(world);
        if (counts.HasError())
            return Outcome::Failure(counts.ErrorValue());
        if (!SceneStateOwnerCountsFit(counts.Value(), state, limits))
            return Outcome::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        try {
            PhysicsSceneStateProjection result;
            if (auto prepared = result.PrepareProjection(world, state, identities, counts.Value()); prepared.HasError())
                return Outcome::Failure(prepared.ErrorValue());
            if (result.RetainedPayloadBytes() > limits.maximumRetainedBytes)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        } catch (const std::length_error &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Physics::Detail
