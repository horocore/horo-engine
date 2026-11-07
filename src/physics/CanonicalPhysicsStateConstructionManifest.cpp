#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateConstructionExport.h"
#include "CanonicalPhysicsStateFpControl.h"

namespace Horo::Physics::Detail {
    namespace {
        Result<void> Fill(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                          const CanonicalPhysicsStateTopologyMap &map, StateDetail::ConstructionManifestOwner &owner,
                          PhysicsStateFpControl current, std::span<const PhysicsStatePersistentOwner> owners) {
            auto shapes = ExportCanonicalConstructionRoots(world, map, owner);
            if (shapes.HasError())
                return Result<void>::Failure(shapes.ErrorValue());
            CanonicalConstructionRows rows{owner.Staging()};
            if (auto bodies = ExportCanonicalConstructionBodies(world, bindings, map, rows); bodies.HasError())
                return bodies;
            if (auto joints = ExportCanonicalConstructionJoints(world, bindings, map, rows); joints.HasError())
                return joints;
            if (auto fixtures = ExportCanonicalConstructionFixtures(world, bindings, map, rows); fixtures.HasError())
                return fixtures;
            const PhysicsStateConstructionManifest manifest{shapes.Value(),
                                                            rows.storage.bodies.first(rows.bodies),
                                                            rows.storage.colliders.first(rows.colliders),
                                                            rows.storage.materials.first(rows.materials),
                                                            rows.storage.joints.first(rows.joints),
                                                            rows.storage.fixtures.first(rows.fixtures),
                                                            rows.storage.queryChildren.first(rows.children),
                                                            rows.storage.owners.first(owners.size())};
            std::ranges::copy(owners, rows.storage.owners.begin());
            return owner.Seal(manifest, current);
        }
    }  // namespace

    /** @copydoc PrepareCanonicalConstructionManifest */
    Result<std::unique_ptr<StateDetail::ConstructionManifestOwner>> PrepareCanonicalConstructionManifest(
        const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings, const CanonicalPhysicsStateTopologyMap &map,
        std::size_t maximumRetainedBytes, std::span<const PhysicsStatePersistentOwner> owners) {
        using Outcome = Result<std::unique_ptr<StateDetail::ConstructionManifestOwner>>;
        auto current = ReadCanonicalPhysicsStateFpControl();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        auto limits = ReadCanonicalConstructionLimits(world, maximumRetainedBytes, owners.size());
        if (limits.HasError())
            return Outcome::Failure(limits.ErrorValue());
        auto owner = StateDetail::ConstructionManifestOwner::Prepare(limits.Value());
        if (owner.HasError())
            return owner;
        if (auto filled = Fill(world, bindings, map, *owner.Value(), current.Value(), owners); filled.HasError())
            return Outcome::Failure(filled.ErrorValue());
        return owner;
    }
}  // namespace Horo::Physics::Detail
