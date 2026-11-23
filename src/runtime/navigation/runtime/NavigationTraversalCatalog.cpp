#include "Horo/Navigation/NavigationTraversal.h"
#include "NavigationTraversalReservations.h"

#include <algorithm>
#include <new>
#include <vector>

namespace Horo::Navigation {
    struct NavigationTraversalCatalog::Implementation final {
        struct Entry final {
            NavigationTraversalLink identity;
            NavigationLinkKind action;
            NavigationTraversalReservations owner;
        };

        /** @brief Finds the one owner of a stable link in the published catalog. */
        Entry *Find(const NavigationLinkId link) noexcept {
            const auto found = std::ranges::lower_bound(entries, link, {}, [](const Entry &entry) {
                return entry.identity.link;
            });
            return found != entries.end() && found->identity.link == link ? std::to_address(found) : nullptr;
        }

        /** @brief Rejects a token from any other topology or gate incarnation. */
        Entry *Find(const NavigationTraversalLink &link) noexcept {
            auto *entry = Find(link.link);
            return entry && entry->identity == link ? entry : nullptr;
        }

        NavigationGeneration topology;
        const NavigationWorldLifecycle *world{};
        const NavigationAgentRegistry *agents{};
        std::vector<Entry> entries;
    };

    /** @copydoc NavigationTraversalCatalog::Create */
    Result<std::unique_ptr<NavigationTraversalCatalog>> NavigationTraversalCatalog::Create(
        const NavigationWorldLifecycle &world, const NavigationAgentRegistry &agents,
        const std::span<const NavigationTraversalAdmission> links) {
        using Output = Result<std::unique_ptr<NavigationTraversalCatalog>>;
        const auto active = world.ActiveDescriptor();
        if (active.HasError())
            return Output::Failure(active.ErrorValue());
        constexpr auto MaximumLinks = 1024U;
        constexpr auto MaximumReceiptSlots = 4096U;
        if (links.empty() || links.size() > MaximumLinks)
            return Output::Failure(MakeError(NavigationErrors::CapacityExceeded));
        try {
            std::vector<NavigationTraversalAdmission> sorted(links.begin(), links.end());
            std::ranges::sort(sorted, {}, &NavigationTraversalAdmission::link);
            if (std::ranges::adjacent_find(sorted, {}, &NavigationTraversalAdmission::link) != sorted.end())
                return Output::Failure(MakeError(NavigationErrors::DescriptorConflict));
            auto catalog = std::make_unique<Implementation>();
            catalog->topology = active.Value().topology;
            catalog->world = &world;
            catalog->agents = &agents;
            catalog->entries.reserve(sorted.size());
            std::size_t slots{};
            for (const auto &link : sorted) {
                slots += link.receiptSlots;
                if (slots > MaximumReceiptSlots)
                    return Output::Failure(MakeError(NavigationErrors::CapacityExceeded));
                const NavigationTraversalLink identity{active.Value(), link.link, link.incarnation};
                auto owner = NavigationTraversalReservations::Create(world, agents, identity, link.action, link.profiles, link.capacity,
                                                                     link.receiptSlots);
                if (owner.HasError())
                    return Output::Failure(std::move(owner).ErrorValue());
                catalog->entries.emplace_back(identity, link.action, std::move(owner).Value());
            }
            return Output::Success(std::make_unique<NavigationTraversalCatalog>(ConstructionKey{}, std::move(catalog)));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationTraversalCatalog::NavigationTraversalCatalog */
    NavigationTraversalCatalog::NavigationTraversalCatalog(ConstructionKey, std::unique_ptr<Implementation> implementation) noexcept
        : implementation_(std::move(implementation)) {}

    NavigationTraversalCatalog::~NavigationTraversalCatalog() = default;

    /** @copydoc NavigationTraversalCatalog::MatchesOwners */
    bool NavigationTraversalCatalog::MatchesOwners(const NavigationWorldLifecycle &world,
                                                   const NavigationAgentRegistry &agents) const noexcept {
        return implementation_->world == &world && implementation_->agents == &agents;
    }

    /** @copydoc NavigationTraversalCatalog::Reserve */
    Result<NavigationTraversalGrant> NavigationTraversalCatalog::Reserve(const NavigationLinkId link, const NavigationGeneration topology,
                                                                         const CrowdAgentHandle agent, const std::uint64_t tick,
                                                                         const std::uint64_t deadline) {
        using Output = Result<NavigationTraversalGrant>;
        if (topology != implementation_->topology)
            return Output::Failure(MakeError(NavigationErrors::StaleSnapshot));
        auto *entry = implementation_->Find(link);
        if (!entry)
            return Output::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
        auto token = entry->owner.Reserve(agent, entry->action, tick, deadline);
        if (token.HasError())
            return Output::Failure(std::move(token).ErrorValue());
        return Output::Success({std::move(token).Value(), entry->action});
    }

    /** @copydoc NavigationTraversalCatalog::Transition */
    bool NavigationTraversalCatalog::Transition(const NavigationTraversalReservation &reservation, const CrowdAgentHandle agent,
                                                const NavigationTraversalState next, const std::uint64_t tick) {
        auto *entry = implementation_->Find(reservation.link);
        return entry && entry->owner.Transition(reservation, agent, next, tick);
    }

    /** @copydoc NavigationTraversalCatalog::IsLive */
    bool NavigationTraversalCatalog::IsLive(const NavigationTraversalReservation &reservation, const CrowdAgentHandle agent,
                                            const std::uint64_t tick) {
        auto *entry = implementation_->Find(reservation.link);
        return entry && entry->owner.IsLive(reservation, agent, tick);
    }

    /** @copydoc NavigationTraversalCatalog::Take */
    std::optional<NavigationTraversalCompletion> NavigationTraversalCatalog::Take(const NavigationTraversalReservation &reservation,
                                                                                  const CrowdAgentHandle agent, const std::uint64_t tick) {
        auto *entry = implementation_->Find(reservation.link);
        if (!entry || !entry->owner.AdvanceAtSafePoint(tick))
            return {};
        return entry->owner.Take(reservation, agent);
    }

    /** @copydoc NavigationTraversalCatalog::Abandon */
    void NavigationTraversalCatalog::Abandon(const NavigationTraversalReservation &reservation, const CrowdAgentHandle agent) noexcept {
        if (auto *entry = implementation_->Find(reservation.link))
            entry->owner.Abandon(reservation, agent);
    }

    /** @copydoc NavigationTraversalCatalog::Retire */
    bool NavigationTraversalCatalog::Retire(const NavigationTraversalLink &link) noexcept {
        if (auto *entry = implementation_->Find(link)) {
            entry->owner.RetireAtSafePoint();
            return true;
        }
        return false;
    }
}  // namespace Horo::Navigation
