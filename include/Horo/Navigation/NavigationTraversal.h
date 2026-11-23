#pragma once

/** @file NavigationTraversal.h
 * @brief Fixed-tick authored link capacity and traversal handoff.
 */

#include "Horo/Navigation/NavigationAgentRegistry.h"
#include "Horo/Navigation/NavigationLinkTypes.h"
#include "Horo/Navigation/NavigationWorldLifecycle.h"

#include <memory>
#include <optional>
#include <span>

namespace Horo::Navigation {
    /** @brief A host-issued link incarnation within an immutable published topology. */
    struct NavigationTraversalLink final {
        NavigationWorldActivationDescriptor activation; /**< Exact world, Scene and topology binding. */
        NavigationLinkId link;                          /**< Stable identity of the admitted authored link. */
        std::uint64_t incarnation{};                    /**< Nonzero, never reused for this link within the topology. */

        constexpr auto operator<=>(const NavigationTraversalLink &) const noexcept = default;
    };

    /** @brief Process-local identity of one accepted traversal operation. */
    struct NavigationTraversalReservation final {
        NavigationTraversalLink link;
        std::uint64_t sequence{}; /**< Nonzero admission sequence within this link incarnation. */

        constexpr auto operator<=>(const NavigationTraversalReservation &) const noexcept = default;
    };

    /** @brief Fixed owner-phase progression of one accepted operation. */
    enum class NavigationTraversalState : std::uint8_t {
        Reserved,
        Entering,
        Active,
        Completed,
        Cancelled,
        Failed
    };

    /** @brief Stable cause retained until the terminal result is consumed. */
    enum class NavigationTraversalEnd : std::uint8_t {
        Completed,
        Cancelled,
        ActionFailed,
        Deadline,
        AgentRetired,
        LinkRetired
    };

    /** @brief One terminal result from the authority that admitted the reservation. */
    struct NavigationTraversalCompletion final {
        NavigationTraversalReservation reservation;
        CrowdAgentHandle agent;
        NavigationTraversalState state;
        NavigationTraversalEnd reason;
    };

    /** @brief Host-admitted action and capacity for one enabled authored link. */
    struct NavigationTraversalAdmission final {
        NavigationLinkId link;
        std::uint64_t incarnation{}; /**< Exact authored/runtime gate incarnation. */
        NavigationLinkKind action{NavigationLinkKind::Jump};
        std::span<const NavigationAgentProfileId> profiles;
        std::uint16_t capacity{1};
        std::uint16_t receiptSlots{2};
    };

    /** @brief Reservation and the only action admitted by its link authority. */
    struct NavigationTraversalGrant final {
        NavigationTraversalReservation reservation;
        NavigationLinkKind action;

        constexpr auto operator<=>(const NavigationTraversalGrant &) const noexcept = default;
    };

    /** @brief Bounded reservation authority for one published Scene and topology.
     * @details All operations are fixed owner-lane calls. The World and agent registry outlive this catalog,
     * which in turn outlives its movement controllers. Link descriptors are copied during preparation.
     */
    class NavigationTraversalCatalog final {
        struct Implementation;

        struct ConstructionKey {
        private:
            friend class NavigationTraversalCatalog;
            ConstructionKey() = default;
        };

    public:
        /** @brief Factory-only construction with the validated catalog storage. */
        NavigationTraversalCatalog(ConstructionKey, std::unique_ptr<Implementation> implementation) noexcept;
        /** @brief Prepares a bounded catalog against the exact active world and registered agent population.
         * @param world Published navigation topology. @param agents Exact Scene's logical agent owner.
         * @param links Unique enabled authored links. @return Owned catalog or a typed invalid or capacity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<NavigationTraversalCatalog>> Create(
            const NavigationWorldLifecycle &world, const NavigationAgentRegistry &agents,
            std::span<const NavigationTraversalAdmission> links);
        NavigationTraversalCatalog(const NavigationTraversalCatalog &) = delete;
        NavigationTraversalCatalog &operator=(const NavigationTraversalCatalog &) = delete;
        ~NavigationTraversalCatalog();

        /** @brief Admits one matching agent to the requested link at the given owner tick.
         * @param link Stable link identity from the installed path. @param topology Exact path topology.
         * @param agent Registered generation. @param tick Current owner tick. @param deadline Exclusive expiry.
         * @return Owned grant, capacity pressure, or a typed missing/stale authority failure.
         */
        [[nodiscard]] Result<NavigationTraversalGrant> Reserve(NavigationLinkId link, NavigationGeneration topology, CrowdAgentHandle agent,
                                                               std::uint64_t tick, std::uint64_t deadline);

        /** @brief Advances the exact reservation at the given owner tick.
         * @param reservation Accepted token. @param agent Original registered agent. @param next Requested state.
         * @param tick Current owner tick. @return True when the transition was accepted.
         */
        [[nodiscard]] bool Transition(const NavigationTraversalReservation &reservation, CrowdAgentHandle agent,
                                      NavigationTraversalState next, std::uint64_t tick);

        /** @brief Checks that the exact grant may still authorize action work at the given tick.
         * @param reservation Accepted token. @param agent Original registered agent. @param tick Current owner tick.
         * @return True only while live in the currently active topology.
         */
        [[nodiscard]] bool IsLive(const NavigationTraversalReservation &reservation, CrowdAgentHandle agent, std::uint64_t tick);

        /** @brief Advances expiry and consumes an exact terminal receipt once.
         * @param reservation Accepted token. @param agent Original registered agent. @param tick Current owner tick.
         * @return Terminal receipt if available.
         */
        [[nodiscard]] std::optional<NavigationTraversalCompletion> Take(const NavigationTraversalReservation &reservation,
                                                                        CrowdAgentHandle agent, std::uint64_t tick);

        /** @brief Revokes an exact grant whose owner no longer consumes results.
         * @param reservation Accepted token. @param agent Original registered agent.
         */
        void Abandon(const NavigationTraversalReservation &reservation, CrowdAgentHandle agent) noexcept;

        /** @brief Permanently withdraws an exact gate incarnation and retains terminal receipts.
         * @param link Stable authored identity and exact topology/gate incarnation.
         * @return True if the catalog owns that exact incarnation.
         */
        [[nodiscard]] bool Retire(const NavigationTraversalLink &link) noexcept;

    private:
        friend class NavigationMovementController;
        /** @brief Matches the concrete World and agent owners borrowed at creation. */
        [[nodiscard]] bool MatchesOwners(const NavigationWorldLifecycle &world, const NavigationAgentRegistry &agents) const noexcept;
        std::unique_ptr<Implementation> implementation_;
    };
}  // namespace Horo::Navigation
