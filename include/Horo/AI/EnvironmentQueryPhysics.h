#pragma once

/** @file EnvironmentQueryPhysics.h
 * @brief Owner-lane Physics capability adapter for tactical clearance and visibility.
 */

#include "Horo/AI/EnvironmentQueryScoring.h"
#include "Horo/Physics/PhysicsQueryEventCapability.h"

#include <array>
#include <cstddef>

namespace Horo::AI {
    /** @brief Exact world binding and completed Physics publication captured by the host. */
    struct QueryPhysicsPublication final {
        Physics::PhysicsQueryEventIdentity identity;
        std::uint64_t sceneGeneration{};
        std::uint64_t revision{};
        std::uint64_t completedTick{};
    };

    /** @brief Hard adapter limit for one ordered visibility request. Physics may impose a smaller allowance. */
    inline constexpr std::size_t MaximumQueryPhysicsVisibilityRays = 32;

    /** @brief Complete visibility evidence in submitted candidate order, with its original Physics publication. */
    struct QueryPhysicsVisibilityResult final {
        QueryPhysicsPublication publication;
        QueryStageId stage;
        std::array<bool, MaximumQueryPhysicsVisibilityRays> clear{};
        std::size_t count{};
    };

    /** @brief One queued tactical test against an exact Physics publication.
     * @details Submit on the Physics owner lane outside fixed-step callbacks. Poll on the query owner
     * lane, serialized with submission, cancellation and destruction. The host supplies exact geometry
     * in the captured Physics origin frame. Reading a completion requires the original live capability.
     */
    class QueryPhysicsProbe final {
    public:
        /** @brief Maximum number of rays for one exposure test, before applying Physics owner limits. */
        static constexpr std::size_t MaximumExposureRays = 8;

        /** @brief Creates an idle probe. */
        QueryPhysicsProbe() = default;
        QueryPhysicsProbe(const QueryPhysicsProbe &) = delete;
        QueryPhysicsProbe &operator=(const QueryPhysicsProbe &) = delete;
        /** @brief Cancels any pending query. */
        ~QueryPhysicsProbe();

        /** @brief Queues one blocking-only ray, resident overlap or inline capsule overlap.
         * @param capability Host-issued Physics read capability.
         * @param publication Exact publication used to form the local geometry.
         * @param stage Authored EQS stage identity.
         * @param geometry Local-frame ray or overlap geometry.
         * @param filter Authorized selectors; the adapter forces blocking-only participation.
         * @return True owns a pending probe, false is capacity backpressure, or a typed error.
         */
        [[nodiscard]] Result<bool> Start(const Physics::PhysicsQueryEventCapability &capability, QueryPhysicsPublication publication,
                                         QueryStageId stage, Physics::PhysicsQueryGeometry geometry, Physics::PhysicsQueryFilter filter);

        /** @brief Queues bounded threat-to-candidate rays as one exposure test.
         * @param capability Host-issued Physics read capability.
         * @param publication Exact publication and coordinate frame of all rays.
         * @param stage Authored EQS stage identity.
         * @param rays Non-empty rays in the captured Physics origin frame, up to MaximumExposureRays.
         * @param filter Shared authorized selectors; only blockers affect exposure.
         * @return True owns the batch, false is capacity backpressure, or a typed error.
         * @details Poll reports whether any ray is clear and the fraction of clear rays. A failed or
         * stale batch has no exposure sample. Physics owns dispatch and budget enforcement.
         */
        [[nodiscard]] Result<bool> StartExposure(const Physics::PhysicsQueryEventCapability &capability,
                                                 QueryPhysicsPublication publication, QueryStageId stage,
                                                 std::span<const Physics::PhysicsRayQuery> rays, Physics::PhysicsQueryFilter filter);

        /** @brief Queues one blocking-only ray per candidate against an exact Physics publication.
         * @param capability Live Physics client. @param publication Captured input and authority.
         * @param stage Exact authored query stage. @param rays Ordered local-frame rays, bounded by MaximumQueryPhysicsVisibilityRays.
         * @param filter Common collision selectors. @return True admits all rays, false for backpressure, or a typed error.
         */
        [[nodiscard]] Result<bool> StartVisibility(const Physics::PhysicsQueryEventCapability &capability,
                                                   QueryPhysicsPublication publication, QueryStageId stage,
                                                   std::span<const Physics::PhysicsRayQuery> rays, Physics::PhysicsQueryFilter filter);

        /** @brief Reads complete ordered evidence while the original Physics publication remains current.
         * @param current Exact live owner observation. @return No value while idle or pending, a complete receipt, or a typed error.
         * @details Read on the Physics owner before advancing its publication. The detached receipt contains no live Physics
         * state and may be passed to the next BlackboardSync by the same query owner.
         */
        [[nodiscard]] Result<std::optional<QueryPhysicsVisibilityResult>> PollVisibility(const QueryPhysicsPublication &current);

        /** @brief Drains a queued completion without waiting.
         * @param current Host-validated binding and publication still authorized for this probe.
         * @return Empty while idle or pending, a clear/blocked sample, or a typed error.
         * @details Changed bindings retire the request. For a single geometry, clear is true with a metric of one.
         */
        [[nodiscard]] Result<std::optional<QueryTestSample>> Poll(const QueryPhysicsPublication &current);
        /** @brief Revokes and releases the queued request without waiting. */
        void Cancel() noexcept;

    private:
        /** @brief Admits one bounded command batch and retains its exact publication identity. */
        [[nodiscard]] Result<bool> Queue(const Physics::PhysicsQueryEventCapability &capability, QueryPhysicsPublication publication,
                                         QueryStageId stage, std::span<const Physics::PhysicsQueryCommand> commands);
        Physics::PhysicsQueryEventCapability capability_;
        QueryPhysicsPublication publication_;
        QueryStageId stage_;
        std::size_t commandCount_{};
        std::optional<Physics::PhysicsQueryBatchHandle> pending_;
    };
}  // namespace Horo::AI
