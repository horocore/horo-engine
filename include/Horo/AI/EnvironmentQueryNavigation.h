#pragma once

/** @file EnvironmentQueryNavigation.h
 * @brief Tactical surface generation and reachability through Navigation-owned results.
 */

#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQueryScoring.h"
#include "Horo/Navigation/NavigationCoordinator.h"

namespace Horo::AI {
    /** @brief Captured Navigation-owned samples in provider order, paired with their exact coordinate frame. */
    struct QueryNavigationSurfaceSource final {
        const Navigation::NavigationSamplePositionResult &samples;
        Navigation::NavigationOutcomeProvenance source;
        Math::WorldCoordinate64 origin;
    };

    /** @brief Projects a bounded Navigation sampling result into canonical EQS points in the provider's stable order.
     * @param surface Already completed Navigation sampling, pinned to the captured input root and origin.
     * @param current Host-authorized current Navigation publication.
     * @param output Bounded destination. Its provenance revision must match the captured Navigation snapshot token.
     * @return Batch with counted invalid coordinates, or a typed stale or limit rejection without modifying output.
     * @details Truncated provider results are rejected as incomplete. Non-finite or overflowing positions are discarded.
     * The adapter does not schedule provider work or retain either publication.
     */
    [[nodiscard]] Result<QueryGenerationResult> GenerateQueryNavigationSurface(const QueryNavigationSurfaceSource &surface,
                                                                               const Navigation::NavigationPathPublication &current,
                                                                               const QueryGeneratorOutput &output);

    /** @brief One exclusive best-effort path request consumed by an EQS host adapter.
     * @details The host supplies exact Navigation coordinates, caller authority and pinned revisions.
     * All methods use the coordinator owner lane. The world is borrowed for submission only.
     * The host dispatches/commits Navigation phases and collects retired work before provider shutdown.
     */
    class QueryNavigationProbe final {
    public:
        /** @brief Binds the existing Navigation owner without allocating a scheduler.
         * @param coordinator Host-owned coordinator issuing revocable path access.
         */
        explicit QueryNavigationProbe(Navigation::NavigationCoordinator &coordinator) noexcept;
        QueryNavigationProbe(const QueryNavigationProbe &) = delete;
        QueryNavigationProbe &operator=(const QueryNavigationProbe &) = delete;
        /** @brief Abandons any retained result reservation without waiting for provider work. */
        ~QueryNavigationProbe();

        /** @brief Submits a bounded path test without dispatching or publishing inline.
         * @param world Exact active Navigation world. @param stage Authored EQS test identity.
         * @param submission Owned request, revisions, caller and deadlines. @param tick Admission tick.
         * @return True owns one request, false means capacity backpressure, or an exact typed error.
         */
        [[nodiscard]] Result<bool> Start(const Navigation::NavigationWorldLifecycle &world, QueryStageId stage,
                                         Navigation::NavigationPathSubmission submission, std::uint64_t tick);

        /** @brief Reads one already owner-published result at BlackboardSync.
         * @param current Host-authorized current Navigation publication.
         * @return Empty while idle or pending, one reachability/cost sample, or an exact typed error.
         * @details Partial/budget-limited paths are incomparable. A proven unreachable path is false.
         * A reachable path supplies true and its finite non-negative traversal cost.
         */
        [[nodiscard]] Result<std::optional<QueryTestSample>> Poll(const Navigation::NavigationPathPublication &current);
        /** @brief Abandons the result reservation and cancels work without waiting. */
        void Cancel() noexcept;

    private:
        /** @brief Checks current host authority before attempting to consume a request. */
        [[nodiscard]] bool IsCurrent(const Navigation::NavigationPathPublication &current) const noexcept;
        Navigation::NavigationCoordinator::PathRequests requests_;
        CancellationToken worldCancellation_;
        Navigation::NavigationWorldActivationDescriptor activation_;
        Navigation::NavigationOutcomeProvenance source_;
        Navigation::NavigationPathCaller caller_;
        Navigation::NavRequestHandle pending_;
        QueryStageId stage_;
    };
}  // namespace Horo::AI
