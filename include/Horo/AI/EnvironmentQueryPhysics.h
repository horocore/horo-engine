#pragma once

/** @file EnvironmentQueryPhysics.h
 * @brief Owner-lane Physics capability adapter for tactical clearance and visibility.
 */

#include "Horo/AI/EnvironmentQueryExecution.h"
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

    /** @brief Borrowed admission input for one candidate-ordered host Physics evaluation. */
    struct QueryPhysicsVisibilityRequest final {
        Runtime::EntityRef querier;
        QueryStageId stage;
        Math::WorldCoordinate64 eye;
        std::span<const Math::WorldCoordinate64> points;
        Physics::PhysicsQueryChannelId channel;
    };

    /** @brief Projects candidate points onto nonzero local rays and retains their original indices. */
    struct QueryPhysicsVisibilityRays final {
        std::array<Physics::PhysicsRayQuery, MaximumQueryPhysicsVisibilityRays> rays{};
        std::array<std::size_t, MaximumQueryPhysicsVisibilityRays> indices{};
        std::size_t count{};
    };

    /** @brief Makes exact finite segment directions before converting to Physics floats.
     * @param request Ordered candidates and their canonical global eye. @param origin Exact Physics origin.
     * @return Detached nonzero rays; coincident candidates stay clear without dispatching a ray.
     */
    [[nodiscard]] Result<QueryPhysicsVisibilityRays> ProjectQueryPhysicsVisibilityRays(const QueryPhysicsVisibilityRequest &request,
                                                                                       Math::WorldCoordinate64 origin);

    /** @brief Bounded owner-side geometry capture for an admitted native query. */
    class IQueryPhysicsVisibilityOwner {
    public:
        virtual ~IQueryPhysicsVisibilityOwner() = default;

        /** @brief Checks that detached evidence still belongs to the same live Physics world.
         * @param scene Exact original Scene. @param publication Originating Physics generation and authority.
         * @return True while the same owner is live. Ordinary Physics tick advancement does not retire the owner.
         */
        [[nodiscard]] virtual bool RetainsVisibilityAuthority(const Runtime::RuntimeSceneView &scene,
                                                              const QueryPhysicsPublication &publication) const noexcept = 0;

        /** @brief Admits all candidates against one exact Physics publication on the owner lane.
         * @param request Valid only during this call.
         * @return Empty on backpressure, complete ordered evidence, or the original typed provider failure.
         */
        [[nodiscard]] virtual Result<std::optional<QueryPhysicsVisibilityResult>> CaptureVisibility(
            const QueryPhysicsVisibilityRequest &request) = 0;
    };

    /** @brief Admits a native lattice with one geometry visibility stage under best-effort scheduling.
     * @param plan Compiled native query. @param registry Registry used to validate it. @param scene Current owner view.
     * @param querier Exact actor. @param inputs Owned execution identity and contexts. @param settings Query limits and timing.
     * @param owner Borrowed fixed-tick provider; it outlives the query and runs only on the owner lane.
     * @return Bounded execution or a typed refusal. No live Physics data is retained or shared through the query cache.
     */
    [[nodiscard]] Result<std::unique_ptr<QueryExecution>> AdmitBuiltinPointVisibilityQuery(
        const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry, const Runtime::RuntimeSceneView &scene,
        Runtime::EntityRef querier, QueryExecutionInputs inputs, QueryExecutionSettings settings, IQueryPhysicsVisibilityOwner &owner);

    /** @brief Prepares a stage that generates the lattice and retains its candidate order for one Physics capture. */
    [[nodiscard]] Result<PreparedPointQueryTest> PrepareBuiltinPointVisibilityTest(
        const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry, const Runtime::RuntimeSceneView &scene,
        Runtime::EntityRef querier, const QueryExecutionInputs &inputs, const QueryExecutionSettings &settings,
        IQueryPhysicsVisibilityOwner &owner);

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
