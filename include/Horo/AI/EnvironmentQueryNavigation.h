#pragma once

/** @file EnvironmentQueryNavigation.h
 * @brief Owner-lane tactical reachability and cost probes through NavigationCoordinator.
 */

#include "Horo/AI/EnvironmentQueryScoring.h"
#include "Horo/Navigation/NavigationCoordinator.h"

namespace Horo::AI {
    /** @brief One exclusive best-effort path request consumed by an EQS host adapter.
     * @details The host supplies exact Navigation coordinates, caller authority and pinned revisions.
     * All methods use the coordinator owner lane. The coordinator and world outlive the probe.
     * The host dispatches/commits Navigation phases and collects retired work before provider shutdown.
     */
    class QueryNavigationProbe final {
    public:
        /** @brief Binds the existing Navigation owner without allocating a scheduler.
         * @param coordinator Host-owned coordinator, which must outlive this object.
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
        Navigation::NavigationCoordinator &coordinator_;
        Navigation::NavigationWorldActivationDescriptor activation_;
        Navigation::NavigationOutcomeProvenance source_;
        Navigation::NavigationPathCaller caller_;
        Navigation::NavRequestHandle pending_;
        QueryStageId stage_;
    };
}  // namespace Horo::AI
