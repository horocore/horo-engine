#include "Horo/AI/SymbolicPlanning.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AITaskScheduler.h"

#include <algorithm>
#include <new>

namespace Horo::AI {
    namespace {
        constexpr auto NoNode = SymbolicPlan::MaximumStates;

        struct Label final {
            std::uint64_t state{};
            std::uint64_t cost{};
            std::size_t parent{NoNode};
            std::size_t action{};
            bool closed{};
        };

        bool PlanningCancelled(const CancellationToken &cancellation, const AiTaskOperationContext *operation) noexcept {
            return cancellation.IsCancellationRequested() || (operation && operation->IsCancellationRequested());
        }
    }  // namespace

    /** @brief Fixed label store for a positive-cost, stable lowest-cost frontier. */
    struct SymbolicDomain::Frontier final {
        std::array<Label, SymbolicPlan::MaximumStates> labels{};
        std::size_t count{1};

        std::size_t Pop() noexcept {
            auto next = NoNode;
            for (std::size_t index = 0; index < count; ++index) {
                const auto &candidate = labels[index];
                if (!candidate.closed && (next == NoNode || candidate.cost < labels[next].cost))
                    next = index;
            }
            if (next != NoNode)
                labels[next].closed = true;
            return next;
        }

        bool Relax(const Label &candidate) noexcept {
            const auto used = std::span{labels}.first(count);
            const auto previous = std::ranges::find(used, candidate.state, &Label::state);
            if (previous != used.end()) {
                if (!previous->closed && candidate.cost < previous->cost)
                    *previous = candidate;
                return true;
            }
            if (count == labels.size())
                return false;
            labels[count++] = candidate;
            return true;
        }

        std::optional<SymbolicPlanStatus> Expand(const std::size_t current, const std::span<const Action> actions,
                                                  AiWorkBudget &budget, const CancellationToken &cancellation,
                                                  const AiTaskOperationContext *operation) noexcept {
            const auto &label = labels[current];
            for (std::size_t index = 0; index < actions.size(); ++index) {
                if (PlanningCancelled(cancellation, operation))
                    return SymbolicPlanStatus::Cancelled;
                if (!budget.Consume(1))
                    return SymbolicPlanStatus::BudgetExhausted;
                const auto &action = actions[index];
                if (!action.preconditions.Matches(label.state))
                    continue;
                const auto next = (label.state & ~action.effects.known) | action.effects.values;
                if (next != label.state && (!budget.Consume(count) || !Relax({next, label.cost + action.cost, current, index})))
                    return SymbolicPlanStatus::BudgetExhausted;
            }
            return std::nullopt;
        }
    };

    std::optional<SymbolicDomain::Mask> SymbolicDomain::Encode(const std::span<const SymbolicLiteral> literals) const noexcept {
        if (literals.size() > factCount_)
            return std::nullopt;
        const auto registered = std::span{facts_}.first(factCount_);
        Mask encoded;
        for (const auto &literal : literals) {
            const auto found = std::ranges::lower_bound(registered, literal.fact);
            if (found == registered.end() || *found != literal.fact)
                return std::nullopt;
            const auto bit = std::uint64_t{1} << (found - registered.begin());
            if (encoded.known & bit)
                return std::nullopt;
            encoded.known |= bit;
            if (literal.value)
                encoded.values |= bit;
        }
        return encoded;
    }

    Result<SymbolicDomain> SymbolicDomain::Capture(const std::span<const SymbolicFactId> facts,
                                                    const std::span<const SymbolicActionSource> actions,
                                                    const AiCapabilityDescriptorSet &tasks,
                                                    const std::optional<BlackboardSchemaId> blackboard) {
        using Domain = Result<SymbolicDomain>;
        if (facts.empty() || facts.size() > MaximumFacts || actions.size() > MaximumActions)
            return Domain::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        SymbolicDomain result;
        result.factCount_ = facts.size();
        auto orderedFacts = std::span{result.facts_}.first(facts.size());
        std::ranges::copy(facts, orderedFacts.begin());
        std::ranges::sort(orderedFacts);
        if (!orderedFacts.front().IsValid() || std::ranges::adjacent_find(orderedFacts) != orderedFacts.end())
            return Domain::Failure(MakeError(AIErrors::IdentityInvalid));

        for (const auto &source : actions) {
            const auto task = tasks.Find(source.task);
            const auto before = result.Encode(source.preconditions);
            const auto after = result.Encode(source.effects);
            if (!source.action.IsValid() || source.cost == 0 || !task || (task->blackboard && task->blackboard != blackboard) ||
                !before || !after || after->known == 0)
                return Domain::Failure(MakeError(AIErrors::TaskContextInvalid));
            result.actions_[result.actionCount_++] = {{source.action, source.task}, source.cost, *before, *after};
        }
        const auto identity = [](const Action &action) { return action.identity.action; };
        auto orderedActions = std::span{result.actions_}.first(result.actionCount_);
        std::ranges::sort(orderedActions, {}, identity);
        if (std::ranges::adjacent_find(orderedActions, {}, identity) != orderedActions.end())
            return Domain::Failure(MakeError(AIErrors::IdentityInvalid));
        return Domain::Success(std::move(result));
    }

    SymbolicPlan SymbolicDomain::Search(const std::span<const SymbolicLiteral> initial, const std::span<const SymbolicLiteral> goal,
                                        const std::size_t work, const CancellationToken &cancellation) const noexcept {
        return SearchImpl(initial, goal, work, cancellation, nullptr);
    }

    SymbolicPlan SymbolicDomain::SearchImpl(const std::span<const SymbolicLiteral> initial,
                                            const std::span<const SymbolicLiteral> goal, const std::size_t work,
                                            const CancellationToken &cancellation, const AiTaskOperationContext *operation) const noexcept {
        using enum SymbolicPlanStatus;
        const auto start = Encode(initial);
        const auto required = Encode(goal);
        const auto stop = [](const SymbolicPlanStatus status) {
            SymbolicPlan result;
            result.status_ = status;
            return result;
        };
        if (!start || !required || work == 0 || work > MaximumWork)
            return stop(InvalidInput);
        Frontier queue;
        queue.labels.front().state = start->values;
        AiWorkBudget budget{work};
        while (!PlanningCancelled(cancellation, operation)) {
            if (!budget.Consume(queue.count))
                return stop(BudgetExhausted);
            const auto current = queue.Pop();
            if (current == NoNode)
                return stop(NoPlan);
            const auto &label = queue.labels[current];
            if (required->Matches(label.state)) {
                SymbolicPlan result;
                result.status_ = Solved;
                result.cost_ = label.cost;
                for (auto index = current; queue.labels[index].parent != NoNode; index = queue.labels[index].parent)
                    result.steps_[result.count_++] = actions_[queue.labels[index].action].identity;
                std::ranges::reverse(std::span{result.steps_}.first(result.count_));
                return PlanningCancelled(cancellation, operation) ? stop(Cancelled) : result;
            }
            if (const auto stopped = queue.Expand(current, std::span{actions_}.first(actionCount_), budget, cancellation, operation))
                return stop(*stopped);
        }
        return stop(Cancelled);
    }

    Result<std::shared_ptr<SymbolicPlanningJob>> SymbolicPlanningJob::Create(
        std::shared_ptr<const SymbolicDomain> domain, const std::span<const SymbolicLiteral> initial,
        const std::span<const SymbolicLiteral> goal, const std::size_t work) {
        using Job = Result<std::shared_ptr<SymbolicPlanningJob>>;
        if (!domain || !domain->Encode(initial) || !domain->Encode(goal) || work == 0 || work > SymbolicDomain::MaximumWork)
            return Job::Failure(MakeError(AIErrors::TaskContextInvalid));
        try {
            auto job = std::shared_ptr<SymbolicPlanningJob>{new SymbolicPlanningJob};
            job->domain_ = std::move(domain);
            std::ranges::copy(initial, job->initial_.begin());
            std::ranges::copy(goal, job->goal_.begin());
            job->initialCount_ = initial.size();
            job->goalCount_ = goal.size();
            job->work_ = work;
            return Job::Success(std::move(job));
        } catch (const std::bad_alloc &) {
            return Job::Failure(MakeError(AIErrors::AgentCapacityExceeded));
        }
    }

    Result<void> SymbolicPlanningJob::Run(const AiTaskOperationContext &operation, const CancellationToken &cancellation) const {
        if (!operation.taskDefinition.IsValid() || !operation.task.IsValid() || !operation.agent.IsValid() ||
            operation.task.incarnation != operation.agent.incarnation)
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        auto expected = State::Ready;
        if (!state_.compare_exchange_strong(expected, State::Running))
            return Result<void>::Failure(MakeError(AIErrors::TaskTransitionInvalid));
        result_ = domain_->SearchImpl(std::span{initial_}.first(initialCount_), std::span{goal_}.first(goalCount_), work_, cancellation,
                                      &operation);
        state_.store(State::Complete, std::memory_order_release);
        return result_.Status() == SymbolicPlanStatus::Cancelled ? JobCancelled() : Result<void>::Success();
    }

    const SymbolicPlan *SymbolicPlanningJob::Read() const noexcept {
        return state_.load(std::memory_order_acquire) == State::Complete ? &result_ : nullptr;
    }
}  // namespace Horo::AI
