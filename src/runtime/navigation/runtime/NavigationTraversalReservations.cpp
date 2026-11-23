#include "NavigationTraversalReservations.h"

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

namespace Horo::Navigation {
    /** @copydoc NavigationTraversalReservations::IsLive */
    bool NavigationTraversalReservations::IsLive(const NavigationTraversalReservation &reservation, const CrowdAgentHandle agent,
                                                 const std::uint64_t tick) {
        if (!CanMutateAt(tick))
            return false;
        const auto *record = Find(reservation, agent);
        return record && record->IsLive();
    }

    /** @copydoc NavigationTraversalReservations::Abandon */
    void NavigationTraversalReservations::Abandon(const NavigationTraversalReservation &reservation,
                                                  const CrowdAgentHandle agent) noexcept {
        auto *record = Find(reservation, agent);
        if (!record)
            return;
        if (record->IsLive())
            --active_;
        *record = {};
    }

    namespace {
        constexpr std::size_t MaximumTraversalSlots = 256;
        constexpr std::size_t MaximumTraversalProfiles = 64;

        /** @brief Checks the live, retained and profile bounds before any storage is allocated. */
        [[nodiscard]] bool WithinTraversalLimits(const std::size_t capacity, const std::size_t receiptSlots,
                                                 const std::size_t profileCount) noexcept {
            return capacity > 0 && capacity <= receiptSlots && receiptSlots <= MaximumTraversalSlots && profileCount > 0 &&
                   profileCount <= MaximumTraversalProfiles;
        }

        /** @brief Checks that this link targets the currently published Scene and provider topology. */
        [[nodiscard]] bool MatchesLink(const NavigationWorldLifecycle &world, const NavigationTraversalLink &link) {
            const auto activation = world.ActiveDescriptor();
            return activation.HasValue() && activation.Value() == link.activation;
        }

        /** @brief Checks the ordered non-cancellation stages in a traversal. */
        [[nodiscard]] bool IsNextStage(const NavigationTraversalState current, const NavigationTraversalState next) noexcept {
            using enum NavigationTraversalState;
            switch (current) {
                case Reserved:
                    return next == Entering;
                case Entering:
                    return next == Active;
                case Active:
                    return next == Completed;
                case Completed:
                case Cancelled:
                case Failed:
                    return false;
            }
            return false;
        }
    }  // namespace

    /** @copydoc NavigationTraversalReservations::NavigationTraversalReservations */
    NavigationTraversalReservations::NavigationTraversalReservations(const NavigationWorldLifecycle &world,
                                                                     const NavigationAgentRegistry &agents,
                                                                     const NavigationTraversalLink &link, const NavigationLinkKind action,
                                                                     const std::span<const NavigationAgentProfileId> profiles,
                                                                     const std::size_t capacity, const std::size_t receiptSlots)
        : world_(world), agents_(agents), link_(link), action_(action), profiles_(profiles.begin(), profiles.end()), records_(receiptSlots),
          capacity_(capacity) {}

    /** @copydoc NavigationTraversalReservations::Create */
    Result<NavigationTraversalReservations> NavigationTraversalReservations::Create(
        const NavigationWorldLifecycle &world, const NavigationAgentRegistry &agents, const NavigationTraversalLink &link,
        const NavigationLinkKind action, const std::span<const NavigationAgentProfileId> profiles, const std::size_t capacity,
        const std::size_t receiptSlots) {
        if (!link.link.IsValid() || link.incarnation == 0 || action >= NavigationLinkKind::Count)
            return Result<NavigationTraversalReservations>::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
        if (!WithinTraversalLimits(capacity, receiptSlots, profiles.size()))
            return Result<NavigationTraversalReservations>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        if (!std::ranges::all_of(profiles, &NavigationAgentProfileId::IsValid))
            return Result<NavigationTraversalReservations>::Failure(MakeError(NavigationErrors::AgentProfileInvalid));
        const auto binding = agents.ActiveBinding();
        if (const NavigationAgentSceneBinding expected{link.activation.world, link.activation.scene, link.activation.sceneGeneration};
            !MatchesLink(world, link) || !binding.HasValue() || binding.Value() != expected)
            return Result<NavigationTraversalReservations>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        try {
            return Result<NavigationTraversalReservations>::Success(
                NavigationTraversalReservations{world, agents, link, action, profiles, capacity, receiptSlots});
        } catch (const std::bad_alloc &) {
            return Result<NavigationTraversalReservations>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationTraversalReservations::Reserve */
    Result<NavigationTraversalReservation> NavigationTraversalReservations::Reserve(const CrowdAgentHandle agent,
                                                                                    const NavigationLinkKind action,
                                                                                    const std::uint64_t tick,
                                                                                    const std::uint64_t deadline) {
        if (!CanMutateAt(tick))
            return Result<NavigationTraversalReservation>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (deadline <= tick)
            return Result<NavigationTraversalReservation>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        if (action != action_ || !IsAgentCompatible(agent))
            return Result<NavigationTraversalReservation>::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
        if (nextSequence_ == 0)
            return Result<NavigationTraversalReservation>::Failure(MakeError(NavigationErrors::GenerationExhausted));
        if (active_ == capacity_ || std::ranges::any_of(records_, [&](const Record &record) {
            return record.IsLive() && record.agent == agent;
        }))
            return Result<NavigationTraversalReservation>::Failure(MakeError(NavigationErrors::AdmissionRejected));
        const auto free = std::ranges::find_if(records_, [](const Record &record) {
            return record.reservation.sequence == 0;
        });
        if (free == records_.end())
            return Result<NavigationTraversalReservation>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        const NavigationTraversalReservation reservation{link_, nextSequence_++};
        *free = {.reservation = reservation, .agent = agent, .deadline = deadline, .end = {}};
        ++active_;
        return Result<NavigationTraversalReservation>::Success(reservation);
    }

    /** @copydoc NavigationTraversalReservations::Transition */
    bool NavigationTraversalReservations::Transition(const NavigationTraversalReservation &reservation, const CrowdAgentHandle agent,
                                                     const NavigationTraversalState next, const std::uint64_t tick) {
        if (!CanMutateAt(tick))
            return false;
        auto *record = Find(reservation, agent);
        if (!record || record->end)
            return false;
        switch (next) {
            case NavigationTraversalState::Cancelled:
                Finish(*record, next, NavigationTraversalEnd::Cancelled);
                return true;
            case NavigationTraversalState::Failed:
                Finish(*record, next, NavigationTraversalEnd::ActionFailed);
                return true;
            default:
                if (!IsNextStage(record->state, next))
                    return false;
        }
        if (next == NavigationTraversalState::Completed)
            Finish(*record, next, NavigationTraversalEnd::Completed);
        else
            record->state = next;
        return true;
    }

    /** @copydoc NavigationTraversalReservations::AdvanceAtSafePoint */
    bool NavigationTraversalReservations::AdvanceAtSafePoint(const std::uint64_t tick) {
        if (tick < tick_)
            return false;
        tick_ = tick;
        if (!retired_ && !MatchesLink(world_, link_))
            RetireAtSafePoint();
        for (auto &record : records_) {
            if (!record.IsLive())
                continue;
            if (!IsAgentCompatible(record.agent))
                Finish(record, NavigationTraversalState::Cancelled, NavigationTraversalEnd::AgentRetired);
            else if (tick >= record.deadline)
                Finish(record, NavigationTraversalState::Failed, NavigationTraversalEnd::Deadline);
        }
        return true;
    }

    /** @copydoc NavigationTraversalReservations::RetireAtSafePoint */
    void NavigationTraversalReservations::RetireAtSafePoint() noexcept {
        retired_ = true;
        for (auto &record : records_) {
            if (record.IsLive())
                Finish(record, NavigationTraversalState::Cancelled, NavigationTraversalEnd::LinkRetired);
        }
    }

    /** @copydoc NavigationTraversalReservations::Take */
    std::optional<NavigationTraversalCompletion> NavigationTraversalReservations::Take(const NavigationTraversalReservation &reservation,
                                                                                       const CrowdAgentHandle agent) noexcept {
        auto *record = Find(reservation, agent);
        if (!record || !record->end)
            return std::nullopt;
        const NavigationTraversalCompletion result{record->reservation, record->agent, record->state, *record->end};
        *record = {};
        return result;
    }

    /** @copydoc NavigationTraversalReservations::IsAgentCompatible */
    bool NavigationTraversalReservations::IsAgentCompatible(const CrowdAgentHandle agent) const {
        if (agent.world != link_.activation.world)
            return false;
        const auto record = agents_.Find(agent);
        return record.HasValue() && record.Value().enabled && std::ranges::find(profiles_, record.Value().profile) != profiles_.end();
    }

    /** @copydoc NavigationTraversalReservations::CanMutateAt */
    bool NavigationTraversalReservations::CanMutateAt(const std::uint64_t tick) {
        return AdvanceAtSafePoint(tick) && !retired_ && world_.State() == NavigationWorldLifecycleState::Active;
    }

    /** @copydoc NavigationTraversalReservations::Find */
    NavigationTraversalReservations::Record *NavigationTraversalReservations::Find(const NavigationTraversalReservation &reservation,
                                                                                   const CrowdAgentHandle agent) noexcept {
        if (reservation.sequence == 0 || reservation.link != link_)
            return nullptr;
        const auto record = std::ranges::find_if(records_, [&](const Record &candidate) {
            return candidate.reservation.sequence == reservation.sequence && candidate.agent == agent;
        });
        return record != records_.end() ? std::to_address(record) : nullptr;
    }

    /** @copydoc NavigationTraversalReservations::Finish */
    void NavigationTraversalReservations::Finish(Record &record, const NavigationTraversalState state,
                                                 const NavigationTraversalEnd end) noexcept {
        record.state = state;
        record.end = end;
        --active_;
    }
}  // namespace Horo::Navigation
