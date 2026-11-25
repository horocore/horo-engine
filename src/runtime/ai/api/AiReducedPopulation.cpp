#include "Horo/AI/AiReducedPopulation.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <new>
#include <thread>

namespace Horo::AI {
    namespace {
        /** @brief Rejects duplicate or inverted stable identities without sorting caller data. */
        bool StrictlyIncreasing(const auto entries, const auto project) {
            return std::ranges::adjacent_find(entries, std::ranges::greater_equal{}, project) == entries.end();
        }

        /** @brief Compares a coarse axis-aligned radius without overflow at signed world limits. */
        bool InRadius(const std::array<std::int64_t, 3> &position, const std::array<std::int64_t, 3> &anchor,
                      const std::uint64_t radius) noexcept {
            for (std::size_t axis = 0; axis < position.size(); ++axis) {
                const auto [low, high] = std::minmax(position[axis], anchor[axis]);
                if (static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low) > radius)
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @brief Stable identity columns are shared by both representations for this owner incarnation. */
    struct AiReducedPopulation::State final {
        const std::thread::id owner{std::this_thread::get_id()};
        AiReducedPopulationLimits limits;
        std::vector<AgentId> identities;
        std::vector<std::array<std::int64_t, 3>> positions;
        std::vector<std::uint8_t> canonical;
        std::uint64_t revision{1};

        /** @brief Resolves a stable identity without copying any of its state. */
        [[nodiscard]] std::optional<std::size_t> Find(const AgentId agent) const noexcept {
            const auto found = std::ranges::lower_bound(identities, agent);
            if (found == identities.end() || *found != agent)
                return std::nullopt;
            return static_cast<std::size_t>(found - identities.begin());
        }

        /** @brief Checks that a prepared candidate still owns the next publication on this lane. */
        [[nodiscard]] bool CanPublish(const std::uint64_t expected) const noexcept {
            return owner == std::this_thread::get_id() && revision == expected &&
                   revision < std::numeric_limits<std::uint64_t>::max();
        }
    };

    AiReducedPopulation::Transition::Transition(const std::shared_ptr<State> &state, std::vector<Change> changes)
        : state_(state), changes_(std::move(changes)), revision_(state->revision) {}
    AiReducedPopulation::Transition::Transition(Transition &&) noexcept = default;
    AiReducedPopulation::Transition &AiReducedPopulation::Transition::operator=(Transition &&) noexcept = default;
    AiReducedPopulation::Transition::~Transition() = default;

    /** @copydoc AiReducedPopulation::Transition::ValidatePublication */
    Result<void> AiReducedPopulation::Transition::ValidatePublication() const {
        const auto state = state_.lock();
        return state && state->CanPublish(revision_) ? Result<void>::Success()
                                                     : Result<void>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
    }

    /** @copydoc AiReducedPopulation::Transition::Publish */
    Result<void> AiReducedPopulation::Transition::Publish() const {
        const auto state = state_.lock();
        if (!state || !state->CanPublish(revision_))
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        for (const auto &change : changes_) {
            state->canonical[change.slot] = !change.demotion.has_value();
            if (change.demotion)
                state->positions[change.slot] = *change.demotion;
        }
        ++state->revision;
        return Result<void>::Success();
    }

    AiReducedPopulation::AiReducedPopulation(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
    AiReducedPopulation::AiReducedPopulation(AiReducedPopulation &&) noexcept = default;
    AiReducedPopulation &AiReducedPopulation::operator=(AiReducedPopulation &&) noexcept = default;
    AiReducedPopulation::~AiReducedPopulation() = default;

    /** @copydoc AiReducedPopulation::Create */
    Result<AiReducedPopulation> AiReducedPopulation::Create(const AiReducedPopulationLimits limits,
                                                            const std::span<const AiReducedAgent> initial) {
        using Output = Result<AiReducedPopulation>;
        if (limits.maximumAgents == 0 || limits.maximumAgents > AiReducedPopulationLimits::HardAgents ||
            limits.maximumTransitions == 0 || limits.maximumTransitions > AiReducedPopulationLimits::HardTransitions ||
            limits.maximumTransitions > limits.maximumAgents || limits.promotionRadiusMillimeters == 0)
            return Output::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        if (initial.size() > limits.maximumAgents)
            return Output::Failure(MakeError(AIErrors::AgentCapacityExceeded));
        if (!StrictlyIncreasing(initial, &AiReducedAgent::agent) ||
            std::ranges::any_of(initial, [](const AiReducedAgent &row) { return !row.agent.IsValid(); }))
            return Output::Failure(MakeError(AIErrors::DescriptorConflict));
        try {
            auto state = std::make_shared<State>();
            state->limits = limits;
            state->identities.reserve(initial.size());
            state->positions.reserve(initial.size());
            state->canonical.resize(initial.size());
            for (const auto &row : initial) {
                state->identities.push_back(row.agent);
                state->positions.push_back(row.positionMillimeters);
            }
            return Output::Success(AiReducedPopulation{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::BehaviorTreeStorageUnavailable));
        }
    }

    /** @copydoc AiReducedPopulation::EvaluatePromotions */
    Result<std::size_t> AiReducedPopulation::EvaluatePromotions(const std::array<std::int64_t, 3> &anchor,
                                                               const std::span<AiReducedAgent> output) const {
        using Output = Result<std::size_t>;
        if (!state_ || state_->owner != std::this_thread::get_id())
            return Output::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        const auto maximum = state_->limits.maximumTransitions;
        if (output.size() < maximum)
            return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        std::size_t written{};
        for (std::size_t slot = 0; slot < state_->identities.size() && written < maximum; ++slot) {
            if (!state_->canonical[slot] && InRadius(state_->positions[slot], anchor, state_->limits.promotionRadiusMillimeters))
                output[written++] = {state_->identities[slot], state_->positions[slot]};
        }
        return Output::Success(written);
    }

    /** @copydoc AiReducedPopulation::PrepareTransition */
    Result<AiReducedPopulation::Transition> AiReducedPopulation::PrepareTransition(const std::span<const AgentId> promotions,
                                                                                   const std::span<const AiReducedAgent> demotions) const {
        using Output = Result<Transition>;
        if (!state_ || state_->owner != std::this_thread::get_id() || state_->revision == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        const auto maximum = state_->limits.maximumTransitions;
        if (promotions.size() > maximum || demotions.size() > maximum - promotions.size())
            return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        if ((promotions.empty() && demotions.empty()) || !StrictlyIncreasing(promotions, std::identity{}) ||
            !StrictlyIncreasing(demotions, &AiReducedAgent::agent))
            return Output::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        try {
            std::vector<Transition::Change> changes;
            changes.reserve(promotions.size() + demotions.size());
            for (const auto agent : promotions) {
                const auto slot = state_->Find(agent);
                if (!slot || state_->canonical[*slot])
                    return Output::Failure(MakeError(AIErrors::TaskContextInvalid));
                changes.push_back({*slot, std::nullopt});
            }
            for (const auto &agent : demotions) {
                const auto slot = state_->Find(agent.agent);
                if (!slot || !state_->canonical[*slot])
                    return Output::Failure(MakeError(AIErrors::TaskContextInvalid));
                changes.push_back({*slot, agent.positionMillimeters});
            }
            return Output::Success(Transition{state_, std::move(changes)});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::BehaviorTreeStorageUnavailable));
        }
    }
}  // namespace Horo::AI
