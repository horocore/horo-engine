#pragma once

/**
 * @file NavigationPathPolicy.h
 * @brief Owner-thread held-path currentness and bounded repath scheduling.
 */

#include "Horo/Navigation/NavigationBackend.h"
#include "Horo/Navigation/NavigationOutcomes.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>

namespace Horo::Navigation {
    /** @brief Dependency that made a held corridor unusable. */
    enum class NavigationPathInvalidationKind : std::uint8_t {
        None,
        World,
        Tile,
        Topology,
        Filter,
        Link,
        Obstacle,
        Target,
        Profile,
        Origin,
    };

    /** @brief First changed dependency, with Horo-owned identity and expected/observed revisions. */
    struct NavigationPathInvalidation final {
        NavigationPathInvalidationKind kind{NavigationPathInvalidationKind::None};
        std::uint64_t dependency{}; /**< Region ID for Tile, otherwise zero. */
        std::uint64_t expected{};   /**< Revision held by the path. */
        std::uint64_t observed{};   /**< Current revision; zero for missing coverage. */

        [[nodiscard]] constexpr bool IsStale() const noexcept {
            return kind != NavigationPathInvalidationKind::None;
        }

        constexpr auto operator<=>(const NavigationPathInvalidation &) const noexcept = default;
    };

    /** @brief One complete owner-thread observation made before path use or repath admission.
     * @details The owner supplies every retained covered/missing region dependency, including an absence generation
     * for each missing region. This is a bounded dependency slice, not an enumeration of the whole world.
     */
    struct NavigationPathObservation final {
        NavigationOutcomeProvenance provenance;                /**< Atomically captured current world/revisions. */
        std::span<const NavigationCoverageDependency> regions; /**< Ordered covered/absence generations. */
        std::uint64_t linkRevision{};                          /**< Current logical off-mesh link revision. */
        std::uint64_t goalRevision{};                          /**< Current target revision. */
        std::uint64_t tick{};                                  /**< Monotonic simulation tick; zero is valid. */
    };

    /** @brief Fixed-tick coalescing limits; zero disables the corresponding delay. */
    struct NavigationRepathPolicy final {
        std::uint64_t debounceTicks{}; /**< Quiet ticks after the newest dependency/target change. */
        std::uint64_t cooldownTicks{}; /**< Minimum ticks between accepted submissions. */
    };

    /** @brief Owner-thread request-admission decision, never an inline provider operation. */
    enum class NavigationRepathAction : std::uint8_t {
        None,
        Debouncing,
        CoolingDown,
        Submit
    };

    /** @brief Bounded single-slot repath request for the newest observed target and source. */
    struct NavigationRepathDecision final {
        NavigationRepathAction action{NavigationRepathAction::None};
        NavigationPathInvalidation cause;
        std::uint64_t goalRevision{};
        std::uint64_t sourceTopology{};
        bool forced{};
    };

    /**
     * @brief Holds one corridor and one coalesced repath intent on the Navigation owner thread.
     * @details No worker or gameplay caller may mutate this object. CurrentPath requires a fresh atomic observation;
     * a physically retained path is never proof of logical currency. Submission remains the coordinator's responsibility.
     * Region observations are bounded to MaximumNavigationOutcomeCoverageDependencies, and no provider references enter
     * this contract. A new accepted PathId must have a different slot generation or slot to prevent old-path aliasing.
     */
    class NavigationPathPolicy final {
    public:
        /** @brief Construct an empty owner-thread policy. @param policy Fixed debounce/cooldown limits. */
        explicit NavigationPathPolicy(NavigationRepathPolicy policy) noexcept;

        /** @brief Replace the held path only if it is current at this owner-thread observation.
         * @param id Generation-safe path identity assigned by the owner.
         * @param path Complete or explicitly partial provider-neutral corridor to own.
         * @param provenance Exact query result source.
         * @param coverage Complete or partial source dependency evidence.
         * @param linkRevision Link revision captured with the source.
         * @param goalRevision Goal revision captured at request admission.
         * @param current Fresh combined-world observation at publication.
         * @return Success or invalid/stale descriptor; on failure the existing path is unchanged.
         */
        [[nodiscard]] Result<void> Install(PathId id, NavigationPath path, NavigationOutcomeProvenance provenance,
                                           NavigationCoverageEvidence coverage, std::uint64_t linkRevision, std::uint64_t goalRevision,
                                           const NavigationPathObservation &current);

        /** @brief Revalidate the held path and coalesce any new change into the single pending intent.
         * @param current Fresh combined-world observation at the owner phase.
         * @return First invalidated dependency or a descriptor error; invalid input leaves state unchanged.
         */
        [[nodiscard]] Result<NavigationPathInvalidation> Observe(const NavigationPathObservation &current);

        /** @brief Borrow a corridor only if revalidated current at this observation.
         * @param current Fresh combined-world observation; borrow ends before the next policy mutation or owner publication.
         * @return Current path or null. Never returns a stale path as current.
         */
        [[nodiscard]] const NavigationPath *CurrentPath(const NavigationPathObservation &current);

        /** @brief Request one immediate repath independent of debounce/cooldown.
         * @param current Fresh owner-thread observation supplying newest source and goal.
         * @return Success, InvalidWorld after latching a world replacement, or invalid observation.
         * @post No provider work is performed inline.
         */
        [[nodiscard]] Result<void> ForceRepath(const NavigationPathObservation &current);

        /** @brief Inspect when the single coalesced request is ready. @param tick Current monotonic tick.
         * @return Submit, delay reason, or no intent; this call does not consume the intent.
         * @pre Observe a fresh combined-world state at the same tick before this decision.
         */
        [[nodiscard]] NavigationRepathDecision Decide(std::uint64_t tick) const noexcept;

        /** @brief Consume a Submit decision after the coordinator actually accepts the request.
         * @param tick Exact admission tick. @return True only for a ready pending request observed at this tick.
         */
        [[nodiscard]] bool MarkSubmitted(std::uint64_t tick) noexcept;

        /** @brief Retire held corridor and pending intent during world/agent shutdown. */
        void Clear() noexcept;
        /** @brief Return exact held identity, or invalid when no path is installed. */
        [[nodiscard]] PathId Id() const noexcept;
        /** @brief Return the latched held-path/world invalidation for diagnostics. */
        [[nodiscard]] NavigationPathInvalidation LastInvalidation() const noexcept;

    private:
        struct Held final {
            PathId id;
            NavigationPath path;
            NavigationOutcomeProvenance provenance;
            NavigationCoverageEvidence coverage;
            std::uint64_t linkRevision{};
            std::uint64_t goalRevision{};
        };

        struct Pending final {
            NavigationPathInvalidation cause;
            std::uint64_t goalRevision{};
            std::uint64_t sourceTopology{};
            std::uint64_t changedTick{};
            bool forced{};
        };

        /** @brief Reject malformed or over-bound atomic observations. */
        [[nodiscard]] static bool ValidObservation(const NavigationPathObservation &current) noexcept;
        /** @brief Reject time or same-world revision rollback after an earlier owner observation. */
        [[nodiscard]] bool ValidAdvance(const NavigationPathObservation &current) const noexcept;
        /** @brief Find the first changed source dependency in deterministic precedence. */
        [[nodiscard]] static NavigationPathInvalidation Compare(const Held &held, const NavigationPathObservation &current) noexcept;
        /** @brief Replace the single pending intent with the newest observed goal and source. */
        void Queue(NavigationPathInvalidation cause, const NavigationPathObservation &current, bool forced) noexcept;

        NavigationRepathPolicy policy_;
        std::optional<Held> held_;
        std::optional<Pending> pending_;
        NavigationPathInvalidation lastInvalidation_;
        std::optional<std::uint64_t> lastSubmittedTick_;
        std::optional<std::uint64_t> lastObservedTick_;
        std::optional<NavigationOutcomeProvenance> lastObservedProvenance_;
        std::uint64_t lastObservedLinkRevision_{};
        std::uint64_t lastObservedGoalRevision_{};
        std::uint64_t lastObservedTopology_{};
    };
}  // namespace Horo::Navigation
