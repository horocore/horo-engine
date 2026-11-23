#include "Horo/Gameplay/GameplayFormationMovement.h"

#include <algorithm>
#include <new>

namespace Horo::Gameplay {
    namespace {
        using Report = Result<GameplayFormationMovementReport>;
        using Navigation::CrowdAgentHandle;
        using Navigation::NavigationWorldId;

        /** @brief Checks that a supplied roster has one entry per exact agent. */
        bool ValidBindings(const std::span<const GameplayFormationMovementBinding> members, const NavigationWorldId world) {
            for (std::size_t index = 0; index < members.size(); ++index) {
                const auto agent = members[index].agent;
                const auto previous = members.first(index);
                if (!agent.IsValid() || agent.world != world ||
                    std::ranges::find(previous, agent, &GameplayFormationMovementBinding::agent) != previous.end())
                    return false;
            }
            return true;
        }

        /** @brief Rejects a malformed result before withdrawing any live movement. */
        bool ValidGoals(const GameplayFormationResolution &resolved) {
            const auto goals = std::span{resolved.goals};
            for (std::size_t index = 0; index < goals.size(); ++index) {
                const auto &goal = goals[index];
                const auto previous = goals.first(index);
                if (!goal.agent.IsValid() || goal.agent.world != resolved.source.world || !Math::IsFinite(goal.movementTarget) ||
                    std::ranges::find(previous, goal.agent, &GameplayFormationGoal::agent) != previous.end() ||
                    std::ranges::find(previous, goal.slot, &GameplayFormationGoal::slot) != previous.end() ||
                    std::ranges::find(resolved.unassigned, goal.agent) != resolved.unassigned.end())
                    return false;
            }
            return true;
        }

        /** @brief Checks bounded input and the actual active topology ahead of the apply side effects. */
        Result<void> ValidateApplication(const GameplayFormationResolution &resolved, const Navigation::NavigationWorldLifecycle &world,
                                         const std::span<const GameplayFormationMovementBinding> members) {
            using namespace Navigation;
            if (constexpr auto maximum = GameplayFormationRequest::MaximumMembers;
                members.size() > maximum || resolved.goals.size() > maximum || resolved.unassigned.size() > maximum - resolved.goals.size())
                return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
            if (const auto active = world.Acquire(resolved.source.world);
                active.HasError() || active.Value().Descriptor() != resolved.source)
                return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
            if (!ValidBindings(members, resolved.source.world) || !ValidGoals(resolved))
                return Result<void>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc GameplayFormationMovement::GameplayFormationMovement */
    GameplayFormationMovement::GameplayFormationMovement(const GameplayFormationId group) noexcept
        : group_(group), owner_(std::this_thread::get_id()) {}

    /** @copydoc GameplayFormationMovement::~GameplayFormationMovement */
    GameplayFormationMovement::~GameplayFormationMovement() {
        if (active_)
            active_->RequestCancellation();
    }

    /** @copydoc GameplayFormationMovement::Apply */
    Report GameplayFormationMovement::Apply(const GameplayFormationResolution &resolved, const std::uint64_t activeRevision,
                                            const Navigation::NavigationWorldLifecycle &world,
                                            const std::span<const GameplayFormationMovementBinding> members,
                                            const CancellationToken &cancellation) {
        using namespace Navigation;
        if (std::this_thread::get_id() != owner_)
            return Report::Failure(MakeError(NavigationCapabilityErrors::WrongThread));
        if (retired_)
            return Report::Failure(MakeError(NavigationCapabilityErrors::ScopeRevoked));
        if (!group_.IsValid() || resolved.group != group_ || resolved.revision != activeRevision || activeRevision <= revision_)
            return Report::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (cancellation.IsCancellationRequested())
            return Report::Failure(MakeError(NavigationErrors::QueryCancelled));
        if (const auto valid = ValidateApplication(resolved, world, members); valid.HasError())
            return Report::Failure(valid.ErrorValue());

        try {
            GameplayFormationMovementReport result;
            result.started.reserve(resolved.goals.size());
            result.refused.reserve(resolved.goals.size());
            result.unassigned.reserve(resolved.unassigned.size() + resolved.goals.size());
            result.unassigned.assign(resolved.unassigned.begin(), resolved.unassigned.end());
            CancellationSource next{cancellation};
            if (active_)
                active_->RequestCancellation();
            active_ = std::move(next);
            revision_ = activeRevision;
            StartMembers(resolved, members, result);
            return Report::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            if (revision_ == activeRevision)
                Retire();
            return Report::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc GameplayFormationMovement::StartMembers */
    void GameplayFormationMovement::StartMembers(const GameplayFormationResolution &resolved,
                                                 const std::span<const GameplayFormationMovementBinding> members,
                                                 GameplayFormationMovementReport &result) const {
        const auto token = active_->Token();
        for (const auto &goal : resolved.goals) {
            const auto found = std::ranges::find(members, goal.agent, &GameplayFormationMovementBinding::agent);
            if (found == members.end() || !found->navigation || !found->navigation->MatchesFormationAgent(goal.agent)) {
                result.unassigned.push_back(goal.agent);
                continue;
            }
            auto started = found->navigation->SubmitFormationSlot(resolved, group_, revision_, token);
            if (started.HasValue())
                result.started.push_back(goal.agent);
            else
                result.refused.emplace_back(goal.agent, std::move(started).ErrorValue());
        }
        if (!result.started.empty())
            result.availability = result.unassigned.empty() && result.refused.empty() ? GameplayFormationAvailability::Complete
                                                                                      : GameplayFormationAvailability::Degraded;
    }

    /** @copydoc GameplayFormationMovement::Retire */
    void GameplayFormationMovement::Retire() noexcept {
        if (std::this_thread::get_id() != owner_)
            return;
        retired_ = true;
        if (active_)
            active_->RequestCancellation();
        active_.reset();
    }
}  // namespace Horo::Gameplay
