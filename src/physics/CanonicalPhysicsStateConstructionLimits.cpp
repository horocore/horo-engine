#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateConstructionExport.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        Result<void> AddRows(std::uint32_t &count, std::size_t additional) {
            if (additional > std::numeric_limits<std::uint32_t>::max() - count)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            count += static_cast<std::uint32_t>(additional);
            return Result<void>::Success();
        }

        Result<void> CollisionCounts(const CanonicalWorld &world, PhysicsStateConstructionManifestLimits &limits) {
            for (const auto &body : world.scene.bodies) {
                if (!body.collision)
                    continue;
                if (auto added = AddRows(limits.maximumColliders, body.collision->colliders.size()); added.HasError())
                    return added;
                for (const auto &collider : body.collision->colliders)
                    if (auto added = AddRows(limits.maximumMaterials, collider.materials.size()); added.HasError())
                        return added;
            }
            return Result<void>::Success();
        }

        Result<void> QueryCounts(const CanonicalWorld &world, PhysicsStateConstructionManifestLimits &limits) {
            for (const auto &fixture : world.query.fixtures)
                if (const auto *compound = std::get_if<PhysicsCompoundShapeDescriptor>(&fixture.descriptor.shape))
                    if (auto added = AddRows(limits.maximumQueryChildren, compound->children.size()); added.HasError())
                        return added;
            return Result<void>::Success();
        }

        Result<void> InventoryCounts(const CanonicalWorld &world, PhysicsStateConstructionManifestLimits &limits) {
            if (auto count = AddRows(limits.maximumBodies, world.scene.bodies.size()); count.HasError())
                return count;
            if (auto count = AddRows(limits.maximumJoints, world.scene.constraints.size()); count.HasError())
                return count;
            if (auto count = AddRows(limits.maximumFixtures, world.query.fixtures.size()); count.HasError())
                return count;
            limits.shapes.maximumRoots = 0;
            if (auto count = AddRows(limits.shapes.maximumRoots, world.scene.shapes.size()); count.HasError())
                return count;
            return AddRows(limits.shapes.maximumRoots, world.query.fixtures.size());
        }

        bool AddBytes(std::size_t count, std::size_t stride, std::size_t maximum, std::size_t &bytes) noexcept {
            if (count > (maximum - bytes) / stride)
                return false;
            bytes += count * stride;
            return true;
        }

        Result<std::size_t> WireBound(const PhysicsStateConstructionManifestLimits &l, std::size_t maximum) {
            using Outcome = Result<std::size_t>;
            // Explicit canonical fields: HPC1 prefix40 + SHP1 prefix16; maximum analytic node is FP9+kind1+tag1+plane16.
            constexpr std::size_t Prefix = StateDetail::ConstructionManifestHeader::Bytes + 16;
            if (maximum < Prefix)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            std::size_t bytes = Prefix;
            if (!AddBytes(l.shapes.maximumNodes, 27, maximum, bytes) || !AddBytes(l.shapes.maximumEdges, 40, maximum, bytes) ||
                !AddBytes(l.shapes.maximumRoots, 12, maximum, bytes))
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            const std::array<std::size_t, 7> counts{l.maximumBodies,   l.maximumColliders,     l.maximumMaterials, l.maximumJoints,
                                                    l.maximumFixtures, l.maximumQueryChildren, l.maximumOwners};
            for (std::size_t i = 0; i < counts.size(); ++i)
                if (!AddBytes(counts[i], StateDetail::ConstructionRowBytes[i], maximum, bytes))
                    return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(bytes);
        }
    }  // namespace

    /** @brief Computes actual immutable row counts and independently bounded wire/storage reservation before allocation. */
    Result<PhysicsStateConstructionManifestLimits> ReadCanonicalConstructionLimits(const CanonicalWorld &world,
                                                                                   std::size_t maximumRetainedBytes,
                                                                                   std::size_t ownerCount) {
        using Outcome = Result<PhysicsStateConstructionManifestLimits>;
        const auto *pool = world.owner.constructionRecipes;
        if (!pool)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        PhysicsStateConstructionManifestLimits limits{.shapes = pool->Limits(), .maximumRetainedBytes = maximumRetainedBytes};
        if (auto count = InventoryCounts(world, limits); count.HasError())
            return Outcome::Failure(count.ErrorValue());
        if (limits.shapes.maximumRoots > pool->Limits().maximumRoots)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (auto counts = CollisionCounts(world, limits); counts.HasError())
            return Outcome::Failure(counts.ErrorValue());
        if (auto counts = QueryCounts(world, limits); counts.HasError())
            return Outcome::Failure(counts.ErrorValue());
        if (auto owners = AddRows(limits.maximumOwners, ownerCount); owners.HasError())
            return Outcome::Failure(owners.ErrorValue());
        auto wire = WireBound(limits, maximumRetainedBytes);
        if (wire.HasError())
            return Outcome::Failure(wire.ErrorValue());
        limits.maximumEncodedBytes = wire.Value();
        return Outcome::Success(limits);
    }
}  // namespace Horo::Physics::Detail
