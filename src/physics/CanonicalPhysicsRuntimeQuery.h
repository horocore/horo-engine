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

    /** @brief Resolves a native compound path to copied stable child metadata; null for a primitive or invalid path. */
    [[nodiscard]] const PhysicsCompoundChild *ResolveCanonicalFixtureChild(const CanonicalQueryFixtureRecord &fixture,
                                                                           JPH::SubShapeID subshape) noexcept;

    /** @brief Fixed native collector storage reused by one owner-thread canonical world. */
    struct CanonicalQueryStorage final {
        std::array<JPH::CastRayCollector::ResultType, MaximumPhysicsQueryHits> rayQueryResults{};
        std::array<JPH::CollidePointCollector::ResultType, MaximumPhysicsQueryHits> pointQueryResults{};
        std::array<JPH::CollideShapeCollector::ResultType, MaximumPhysicsQueryHits> overlapQueryResults{};
        std::array<JPH::CastShapeCollector::ResultType, MaximumPhysicsQueryHits> sweepQueryResults{};
        std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> queryCandidates{};
    };

    struct CanonicalWorld;
    struct CanonicalSceneBodyRecord;
    struct CanonicalSceneShapeRecord;
    struct CanonicalSimulationTable;

    /** @brief Narrow query view over world-owned native and stable fixture state.
     * @details All spans and table references borrow the joined synchronous world operation. The owner
     * prevents publication, retirement and shutdown until the operation returns; callers cannot retain this view.
     */
    struct CanonicalQueryAccess final {
        JPH::PhysicsSystem &system;
        std::vector<CanonicalQueryFixtureRecord> &fixtures;
        std::vector<std::size_t> &nativeFixtureIndices;
        std::uint32_t maximumFixtures{};
        std::uint32_t &nextFixtureSlot;
        std::uint32_t &nextFixtureGeneration;
        std::uint64_t &querySchemaGeneration;
        CanonicalQueryStorage &storage;
        std::span<const CanonicalSceneBodyRecord> sceneBodies;
        std::span<const std::size_t> nativeSceneBodyIndices;
        const CanonicalSimulationTable &simulation;
        std::span<const CanonicalSceneShapeRecord> sceneShapes;
    };

    /** @brief Borrows one joined world operation's resident records, native indices and immutable simulation authority.
     * @param world Live owner whose operation prevents publication, retirement and shutdown throughout use.
     * @return Non-owning synchronous access; neither the result nor its spans may escape the joined operation.
     */
    [[nodiscard]] CanonicalQueryAccess MakeQueryAccess(CanonicalWorld &world);

    /** @brief Resolves an indexed resident body only for the exact native ID and Horo world generation.
     * @return Borrow valid only during the access view's joined synchronous operation, or null for stale/foreign IDs.
     */
    [[nodiscard]] const CanonicalSceneBodyRecord *ResolveCanonicalSceneBody(const CanonicalQueryAccess &access, PhysicsWorldId owner,
                                                                            JPH::BodyID nativeBody) noexcept;

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
