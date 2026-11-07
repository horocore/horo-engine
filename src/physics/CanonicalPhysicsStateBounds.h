#pragma once

#include "CanonicalPhysicsStateLayout.h"
#include "Horo/Physics/PhysicsWorldSettings.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Horo::Physics::Detail {
    /** @brief Pinned recorder scratch plan, separate from retained snapshot payload
     * and native candidate costs. */
    struct CanonicalPhysicsStateBounds final {
        std::size_t cacheBytes{}, pairEntries{}, manifoldEntries{}, temporaryBytes{}, recorderBytes{};
    };

    /** @brief Derives conservative bounds from the REAL shared cache allocation,
     * not merely its hash bucket counts.
     * @details Pinned Jolt ContactConstraintManager.cpp295/734: BPKeyValue=40
     * bytes, MKeyValue=80 bytes, contactPoint=28 bytes, four-point maximum. Cache
     * allocator=40*max(pairCapacity,4)+164*max(contactCapacity,4), saturated to
     * u32. Zero-point/CCD entries still occupy the complete 80-byte MKeyValue.
     * Array.h230 grows 1,2,4,...; cumulative capacities before final free are <4n.
     * SaveState uses one body vector, one pair vector, one fresh manifold vector
     * per pair, then one all-manifold vector for CCD. For those pointer arrays
     * <=B+4P+8M pointer slots and <=1+P+2M allocations suffice. Each allocation
     * reserves a 32-byte private provenance header plus at most 15 alignment bytes,
     * rounded here to48. RestoreState PhysicsSystem.cpp2942 also grows a BodyID
     * vector for broadphase AABB refresh. Its total geometric capacity is <4B
     * entries and at most B allocations. The body term below covers BOTH save and
     * restore (using pointer width, at least BodyID width), including every
     * monotonic-arena growth hole. No StateRecorderFilter is installed; selected
     * arrays move storage instead of duplicating it. The upper bound includes all
     * geometric reallocations even when the arena does not reclaim holes.
     */
    inline Result<CanonicalPhysicsStateBounds> PlanCanonicalPhysicsStateBounds(const PhysicsWorldSettings &settings,
                                                                               const std::size_t maximumBytes) {
        const auto &values = settings.Values();
        const std::uint64_t pairs = std::max(values.budgets.maximumContactPairs, 4U);
        const std::uint64_t constraints = std::max(values.budgets.maximumContactConstraints, 4U);
        const auto cache = std::min(pairs * CanonicalStatePairEntryBytes + constraints * CanonicalStateWorstManifoldBytes,
                                    std::uint64_t{std::numeric_limits<std::uint32_t>::max()});
        const std::uint64_t p = cache / CanonicalStatePairEntryBytes;
        const std::uint64_t m = cache / CanonicalStateManifoldEntryBytes;
        const std::uint64_t bodies = values.world.capacity.maximumBodies;
        const std::uint64_t joints = values.world.capacity.maximumConstraints;
        const std::uint64_t temporary = 4 * bodies * sizeof(void *) + (4 * p + 8 * m) * sizeof(void *) + (bodies + p + 2 * m) * 48;
        const std::uint64_t recorder = 64 + bodies * 192 + p * 36 + m * (154 + 16) + joints * 128;
        if (temporary > maximumBytes || recorder > maximumBytes - temporary)
            return Result<CanonicalPhysicsStateBounds>::Failure(MakeError(PhysicsErrors::CapacityExceeded,
                                                                          "Physics state native scratch budget cannot cover the "
                                                                          "complete pinned recorder allocation bound."));
        return Result<CanonicalPhysicsStateBounds>::Success({static_cast<std::size_t>(cache), static_cast<std::size_t>(p),
                                                             static_cast<std::size_t>(m), static_cast<std::size_t>(temporary),
                                                             static_cast<std::size_t>(recorder)});
    }
}  // namespace Horo::Physics::Detail
