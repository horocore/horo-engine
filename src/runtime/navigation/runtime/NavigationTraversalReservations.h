#pragma once

/** @file NavigationTraversalReservations.h
 * @brief Target-private reservations for one admitted off-mesh link incarnation.
 */

#include "Horo/Navigation/NavigationTraversal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Navigation {
    /**
     * @brief Owner-thread state for one admitted off-mesh link incarnation.
     * @details The host supplies the action, allowed profiles and capacity from current link authority, owns the
     * bound world/agent registries (which outlive this object), and calls at the fixed owner safe point. Terminal
     * records release link capacity immediately. Their separate bounded receipt slots are held until consumed.
     */
    class NavigationTraversalReservations final {
    public:
        /** @brief Preallocates bounded reservation and receipt storage for one link.
         * @param world Current published world authority backing this link.
         * @param agents Current logical agent authority for the same activation.
         * @param link Host-qualified link, including the issuing catalog generation.
         * @param action Only authored traversal action admitted by the link.
         * @param profiles Grounded agent profiles admitted by the link, copied before return.
         * @param capacity Maximum simultaneous users across all profiles.
         * @param receiptSlots Total live and terminal slots, at least capacity and at most 256.
         * @return Prepared owner or a typed validation, staleness or capacity error.
         */
        [[nodiscard]] static Result<NavigationTraversalReservations> Create(const NavigationWorldLifecycle &world,
                                                                            const NavigationAgentRegistry &agents,
                                                                            const NavigationTraversalLink &link, NavigationLinkKind action,
                                                                            std::span<const NavigationAgentProfileId> profiles,
                                                                            std::size_t capacity, std::size_t receiptSlots);

        NavigationTraversalReservations(const NavigationTraversalReservations &) = delete;
        NavigationTraversalReservations &operator=(const NavigationTraversalReservations &) = delete;
        NavigationTraversalReservations(NavigationTraversalReservations &&) noexcept = default;
        NavigationTraversalReservations &operator=(NavigationTraversalReservations &&) = delete;

        /** @brief Admits one compatible, registered agent under the current link capacity.
         * @param agent Exact registered agent generation.
         * @param action Requested action, matching the admitted link kind.
         * @param tick Current monotonic owner simulation tick.
         * @param deadline Exclusive final tick; must be later than tick.
         * @return One admitted identity or a typed rejection. AdmissionRejected means live contention.
         */
        [[nodiscard]] Result<NavigationTraversalReservation> Reserve(CrowdAgentHandle agent, NavigationLinkKind action, std::uint64_t tick,
                                                                     std::uint64_t deadline);

        /** @brief Reports admission capacity after expiring the current owner tick. */
        [[nodiscard]] Result<NavigationTraversalAvailability> AvailabilityAtSafePoint(std::uint64_t tick);

        /** @brief Advances an exact live reservation, or publishes its terminal result once.
         * @param reservation Exact identity returned from Reserve.
         * @param agent Exact agent generation that owns the reservation.
         * @param next Entering after Reserved, Active after Entering, Completed after Active, or Cancelled/Failed while live.
         * @param tick Current monotonic owner simulation tick.
         * @return True for an accepted transition; false for stale identities, wrong owner or illegal transitions.
         */
        [[nodiscard]] bool Transition(const NavigationTraversalReservation &reservation, CrowdAgentHandle agent,
                                      NavigationTraversalState next, std::uint64_t tick);

        /** @brief Expires deadlines and agents and retires a replaced or unloaded link before owner publication.
         * @param tick Current monotonic simulation tick.
         * @return Whether the tick was accepted.
         */
        [[nodiscard]] bool AdvanceAtSafePoint(std::uint64_t tick);

        /** @brief Closes admission permanently and cancels every live traversal for this link.
         * @details Use on gate destruction/disabling or shutdown. Drain retained terminal receipts before destroying the owner.
         */
        void RetireAtSafePoint() noexcept;

        /** @brief Consumes a matching terminal result exactly once, including after link or agent retirement.
         * @param reservation Exact admitted identity.
         * @param agent Original admitted agent generation.
         * @param tick Current owner tick. Mismatched owners cannot advance it.
         * @return The terminal receipt or empty for live, consumed or mismatched identities.
         */
        [[nodiscard]] std::optional<NavigationTraversalCompletion> Take(const NavigationTraversalReservation &reservation,
                                                                        CrowdAgentHandle agent, std::uint64_t tick);

        /** @brief Matches an exact live grant after advancing retirement at the owner tick. */
        [[nodiscard]] bool IsLive(const NavigationTraversalReservation &reservation, CrowdAgentHandle agent, std::uint64_t tick);
        /** @brief Releases an exact live or terminal slot without retaining a receipt. */
        void Abandon(const NavigationTraversalReservation &reservation, CrowdAgentHandle agent) noexcept;

    private:
        struct Record final {
            NavigationTraversalReservation reservation;
            CrowdAgentHandle agent;
            std::uint64_t deadline{};
            NavigationTraversalState state{NavigationTraversalState::Reserved};
            std::optional<NavigationTraversalEnd> end;

            /** @brief Whether this retained slot consumes link capacity. */
            [[nodiscard]] bool IsLive() const noexcept {
                return reservation.sequence != 0 && !end;
            }
        };

        /** @brief Owns detached storage after Create validates the complete link contract. */
        NavigationTraversalReservations(const NavigationWorldLifecycle &world, const NavigationAgentRegistry &agents,
                                        const NavigationTraversalLink &link, NavigationLinkKind action,
                                        std::span<const NavigationAgentProfileId> profiles, std::size_t capacity, std::size_t receiptSlots);
        /** @brief Resolves the exact current agent and verifies its admitted profile. */
        [[nodiscard]] bool IsAgentCompatible(CrowdAgentHandle agent) const;
        /** @brief Refreshes retirement before admitting mutations to a running link at the given tick. */
        [[nodiscard]] bool CanMutateAt(std::uint64_t tick);
        /** @brief Resolves a retained record by link authority, admission sequence and agent. */
        [[nodiscard]] Record *Find(const NavigationTraversalReservation &reservation, CrowdAgentHandle agent) noexcept;
        /** @brief Matches the exact live owner before advancing its tick and rechecks after expiry. */
        [[nodiscard]] Record *FindLiveAt(const NavigationTraversalReservation &reservation, CrowdAgentHandle agent, std::uint64_t tick);
        /** @brief Locates an unused receipt slot without changing occupancy. */
        [[nodiscard]] Record *FreeRecord() noexcept;
        /** @brief Releases live capacity while retaining the first terminal result. */
        void Finish(Record &record, NavigationTraversalState state, NavigationTraversalEnd end) noexcept;

        const NavigationWorldLifecycle &world_;
        const NavigationAgentRegistry &agents_;
        NavigationTraversalLink link_;
        NavigationLinkKind action_;
        std::vector<NavigationAgentProfileId> profiles_;
        std::vector<Record> records_;
        std::size_t capacity_;
        std::size_t active_{};
        std::uint64_t tick_{};
        std::uint64_t nextSequence_{1};
        bool retired_{};
    };
}  // namespace Horo::Navigation
