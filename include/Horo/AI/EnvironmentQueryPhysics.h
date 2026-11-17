#pragma once

/** @file EnvironmentQueryPhysics.h
 * @brief Owner-lane Physics capability adapter for tactical clearance and visibility.
 */

#include "Horo/AI/EnvironmentQueryScoring.h"
#include "Horo/Physics/PhysicsQueryEventCapability.h"

namespace Horo::AI {
    /** @brief Exact world binding and completed Physics publication captured by the host. */
    struct QueryPhysicsPublication final {
        Physics::PhysicsQueryEventIdentity identity;
        std::uint64_t sceneGeneration{};
        std::uint64_t revision{};
        std::uint64_t completedTick{};
    };

    /** @brief One exclusive queued ray or overlap test with a blocking-only filter.
     * @details Submit on the Physics owner lane outside fixed-step callbacks. Poll on the query owner
     * lane, serialized with submission, cancellation and destruction. The host supplies exact geometry
     * in the captured Physics origin frame and cancels the probe before retiring its source.
     */
    class QueryPhysicsProbe final {
    public:
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

        /** @brief Drains a queued completion without waiting.
         * @param current Host-validated binding and publication still authorized for this probe.
         * @return Empty while idle or pending, a clear/blocked sample, or a typed error.
         * @details Changed bindings retire the request. Clear is represented as true and a metric of one.
         */
        [[nodiscard]] Result<std::optional<QueryTestSample>> Poll(const QueryPhysicsPublication &current);
        /** @brief Revokes and releases the queued request without waiting. */
        void Cancel() noexcept;

    private:
        QueryPhysicsPublication publication_;
        QueryStageId stage_;
        Physics::PhysicsQueryDescriptor descriptor_;
        std::optional<Physics::PhysicsQueryBatchHandle> pending_;
    };
}  // namespace Horo::AI
