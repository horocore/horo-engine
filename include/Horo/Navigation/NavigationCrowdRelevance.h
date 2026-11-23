#pragma once

/** @file NavigationCrowdRelevance.h
 * @brief Gameplay-driven, per-agent fixed-tick admission for optional crowd work.
 */

#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <optional>

namespace Horo::Navigation {
    /** @brief Activity tiers, ordered from highest to lowest crowd cadence. */
    enum class NavigationCrowdTier : std::uint8_t {
        Full,
        Reduced,
        PathOnly,
        Suspended,
    };

    /** @brief Optional work authorized for this tick. Character retains all movement authority. */
    enum class NavigationCrowdWork : std::uint8_t {
        Avoidance,
        PathOnly,
        Suspended,
    };

    /** @brief Authored distance bands and cadence for one owner-managed agent population. */
    struct NavigationCrowdRelevanceProfile final {
        std::uint64_t fullDistanceMillimeters{};       /**< Inclusive full-cadence distance. */
        std::uint64_t reducedDistanceMillimeters{};    /**< Inclusive reduced-cadence distance. */
        std::uint64_t suspensionDistanceMillimeters{}; /**< Inclusive path-only distance. */
        std::uint32_t reducedIntervalTicks{};          /**< Must be greater than one. */
        std::uint32_t demotionDelayTicks{};            /**< Consecutive elapsed ticks at a lower target tier. */

        /** @brief Checks ordered bands and positive timing. @return True for admitted profile values. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Committed gameplay relevance for one exact registration. */
    struct NavigationCrowdRelevance final {
        std::optional<std::uint64_t> anchorDistanceMillimeters; /**< Missing relevance preserves full cadence. */
        bool activeSimulationRegion{};                          /**< Keeps an agent at least path-only. */
        bool gameplayPinned{};                                  /**< Keeps an agent at full cadence. */
    };

    /** @brief Owner decision for the current tick. */
    struct NavigationCrowdAdmission final {
        NavigationCrowdTier tier{NavigationCrowdTier::Full};
        NavigationCrowdWork work{NavigationCrowdWork::Avoidance};
        bool tierChanged{};
    };

    /** @brief Allocation-free cadence state owned alongside an exact agent registration.
     * @details Promotion is immediate. Demotion requires consecutive authoritative observations of one target tier.
     * A gap or target change restarts the dwell. State controls work admission only and cannot modify registration,
     * route, Character position, or desired velocity. The Scene owner discards this state when the registration retires.
     */
    class NavigationCrowdCadence final {
    public:
        /** @brief Evaluates work admission once for a strictly advancing owner tick.
         * @param profile Admitted gameplay profile for this registration.
         * @param relevance Current gameplay facts from the same committed Scene root.
         * @param tick Positive fixed simulation tick.
         * @return Tier and allowed work, or a typed error leaving this cadence unchanged.
         */
        [[nodiscard]] Result<NavigationCrowdAdmission> Advance(const NavigationCrowdRelevanceProfile &profile,
                                                               const NavigationCrowdRelevance &relevance, std::uint64_t tick);

    private:
        /** @brief Applies immediate promotions and waits for a continuous demotion observation. */
        bool ApplyTier(NavigationCrowdTier desired, std::uint64_t tick, std::uint32_t delay) noexcept;
        /** @brief Returns the work due in the current tier and records any admitted solve tick. */
        NavigationCrowdWork AdmitWork(std::uint64_t tick, std::uint32_t interval, bool changed) noexcept;

        NavigationCrowdTier tier_{NavigationCrowdTier::Full};
        NavigationCrowdTier demotionTarget_{NavigationCrowdTier::Full};
        std::uint64_t demotionSince_{};
        std::uint64_t observedTick_{};
        std::uint64_t lastAvoidanceTick_{};
    };
}  // namespace Horo::Navigation
