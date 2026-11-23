#pragma once

/** @file NavigationCrowdOverload.h
 * @brief Fixed-tick admission and bounded recovery for best-effort crowd work.
 */

#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::Navigation {
    /** @brief Authored solve ceilings independent of renderer state and native avoidance priorities. */
    struct NavigationCrowdWorkBudget final {
        std::size_t maximumAgents{};
        std::size_t maximumAgentsPerGroup{};

        /** @brief Checks that the group ceiling fits within the total. @return True for two positive ordered ceilings. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One distinct agent requesting service in a captured spatial group. */
    struct NavigationCrowdDemand final {
        std::size_t agentIndex{};
        std::size_t groupIndex{};
        std::uint64_t waitingSince{}; /**< First tick of the retained, unserved demand. */
    };

    /** @brief Admission envelope used for the next worker batch. */
    struct NavigationCrowdCapacity final {
        std::size_t agents{};
        std::size_t jobs{};
    };

    /** @brief Owner-managed overload recovery with one adjustment per observed batch.
     * @details Pressure halves the attempted envelope down to a single agent and a single job.
     * A complete batch restores one slot per dimension up to the authored ceiling. The owner
     * retains the latest demand and makes at most one attempt for each fixed tick.
     */
    class NavigationCrowdRecoveryBudget final {
    public:
        /** @brief Bounds the next attempt by both host limits and the learned envelope.
         * @param maximum Host-selected positive ceilings.
         * @return Positive next capacity within those ceilings.
         */
        [[nodiscard]] NavigationCrowdCapacity Limit(NavigationCrowdCapacity maximum) const noexcept;
        /** @brief Records one observed attempt without retrying the current tick.
         * @param attempted Positive envelope actually attempted, including the partition width.
         * @param saturated True for admission pressure or a missed collection cutoff.
         */
        void Observe(NavigationCrowdCapacity attempted, bool saturated) noexcept;

    private:
        NavigationCrowdCapacity next_{};
    };

    /** @brief Orders the oldest unserved agents and packs the selected prefix in place.
     * @param demands One distinct demand per agent, each from a live group and positive observation tick.
     * @param groupCounts Reusable owner scratch with at least one slot per captured group. All slots are cleared.
     * @param budget Positive total and per-group limits.
     * @return Length of the selected prefix, or a typed input error without sorting the demands.
     * @details Groups tie by capture order. Within an equal-age group, stable agent index breaks ties.
     * The caller retains wait age until a valid result is consumed, so capture changes and worker pressure cannot reset it.
     */
    [[nodiscard]] Result<std::size_t> SelectNavigationCrowdDemand(std::span<NavigationCrowdDemand> demands,
                                                                  std::span<std::size_t> groupCounts, NavigationCrowdWorkBudget budget);
}  // namespace Horo::Navigation
