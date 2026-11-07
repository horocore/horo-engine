#pragma once

#include <Jolt/Jolt.h>

// Jolt subsidiary declarations require its root configuration first.
#include <Jolt/Core/LockFreeHashMap.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vector.h>
#include <Jolt/Physics/Body/BodyPair.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Horo::Physics::Detail {
    /** @brief Size-only layout witnesses for the pinned PRIVATE cache fields, not
     * runtime state or serialized records.
     * @details The actual Jolt source independently asserts
     * sizeof(CachedBodyPair)==28, sizeof(CachedManifold)==60 and alignment4
     * (ContactConstraintManager.h317–357). These witnesses use the exact public
     * native field types with the actual public KeyValue template. The verified
     * pinned archive and reviewed adapter grammar are required together; a changed
     * source layout requires a solver-schema review even if coincidental sizes
     * still match. No cache object is reinterpreted as one of these witnesses. They
     * establish checked upper-bound arithmetic only.
     */
    struct CanonicalStatePointLayout final {
        JPH::Float3 position1, position2;
        float lambda;
    };

    struct CanonicalStatePairLayout final {
        JPH::Float3 relativePosition, relativeRotation;
        std::uint32_t next;
    };

    struct CanonicalStateManifoldLayout final {
        std::uint32_t next;
        JPH::Float3 normal;
        JPH::Vector<2> friction;
        float angularFriction;
        std::atomic<std::uint16_t> flags;
        std::uint16_t count;
        CanonicalStatePointLayout point;
    };

    using CanonicalStatePairEntryLayout = JPH::LockFreeHashMap<JPH::BodyPair, CanonicalStatePairLayout>::KeyValue;
    using CanonicalStateManifoldEntryLayout = JPH::LockFreeHashMap<JPH::SubShapeIDPair, CanonicalStateManifoldLayout>::KeyValue;

    static_assert(sizeof(void *) == 8, "The pinned state allocation bound requires "
                                       "a qualified 64-bit CPU target.");
    static_assert(sizeof(JPH::Vec3) == 16 && sizeof(JPH::RVec3) == 16 && sizeof(JPH::Quat) == 16);
    static_assert(sizeof(JPH::Float3) == 12 && sizeof(JPH::Vector<2>) == 8);
    static_assert(sizeof(CanonicalStatePointLayout) == 28 && alignof(CanonicalStatePointLayout) == 4);
    static_assert(sizeof(CanonicalStatePairLayout) == 28 && alignof(CanonicalStatePairLayout) == 4);
    static_assert(sizeof(CanonicalStateManifoldLayout) == 60 && alignof(CanonicalStateManifoldLayout) == 4);
    static_assert(sizeof(CanonicalStatePairEntryLayout) == 40 && sizeof(CanonicalStateManifoldEntryLayout) == 80);
    static_assert(alignof(CanonicalStatePairEntryLayout) <= 16 && alignof(CanonicalStateManifoldEntryLayout) <= 16);

    inline constexpr std::size_t CanonicalStatePairEntryBytes = sizeof(CanonicalStatePairEntryLayout);
    inline constexpr std::size_t CanonicalStateManifoldEntryBytes = sizeof(CanonicalStateManifoldEntryLayout);
    inline constexpr std::size_t CanonicalStateWorstManifoldBytes =
        CanonicalStateManifoldEntryBytes + 3 * sizeof(CanonicalStatePointLayout);
}  // namespace Horo::Physics::Detail
