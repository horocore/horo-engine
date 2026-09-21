#pragma once

/** @file CanonicalPhysicsRuntimeQuery.h
 * @brief Private native query state and adapter declarations for the canonical Physics runtime.
 */

#include "CanonicalPhysicsRuntime.h"

#include <Jolt/Jolt.h>

// Jolt subsidiary headers require its root definitions first.
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Physics::Detail {
    /** @brief Native fixture record retained only by one owner-thread canonical world. */
    struct CanonicalQueryFixtureRecord final {
        PhysicsQueryFixture fixture;
        PhysicsQueryFixtureDescriptor descriptor;
        JPH::BodyID nativeBody;
        JPH::Ref<JPH::Shape> shape;
    };

    /** @brief Fixed native collector storage reused by one owner-thread canonical world. */
    struct CanonicalQueryStorage final {
        std::array<JPH::CastRayCollector::ResultType, MaximumPhysicsQueryHits> rayQueryResults{};
        std::array<JPH::CollidePointCollector::ResultType, MaximumPhysicsQueryHits> pointQueryResults{};
        std::array<JPH::CollideShapeCollector::ResultType, MaximumPhysicsQueryHits> overlapQueryResults{};
        std::array<JPH::CastShapeCollector::ResultType, MaximumPhysicsQueryHits> sweepQueryResults{};
        std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> queryCandidates{};
    };

    /** @brief Narrow query view over world-owned native and stable fixture state. */
    struct CanonicalQueryAccess final {
        JPH::PhysicsSystem &system;
        std::vector<CanonicalQueryFixtureRecord> &fixtures;
        std::vector<std::size_t> &nativeFixtureIndices;
        std::uint32_t maximumFixtures{};
        std::uint32_t &nextFixtureSlot;
        std::uint32_t &nextFixtureGeneration;
        std::uint64_t &querySchemaGeneration;
        CanonicalQueryStorage &storage;
    };

    /** @brief Creates one validated native shape/body through the private query view. */
    [[nodiscard]] Result<PhysicsQueryFixture> CreateCanonicalQueryFixtureFromAccess(CanonicalQueryAccess &access, PhysicsWorldId owner,
                                                                                    const PhysicsQueryFixtureDescriptor &fixture);
    /** @brief Retires one exact native fixture through the private query view. */
    [[nodiscard]] Result<void> DestroyCanonicalQueryFixtureFromAccess(CanonicalQueryAccess &access, const PhysicsQueryFixture &fixture);
    /** @brief Executes one validated immediate query through fixed native storage. */
    [[nodiscard]] Result<PhysicsQueryResult> ExecuteCanonicalQueryFromAccess(CanonicalQueryAccess &access,
                                                                             const PhysicsQueryDescriptor &descriptor,
                                                                             std::span<PhysicsQueryHit> hits);
}  // namespace Horo::Physics::Detail
