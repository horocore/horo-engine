#include "Horo/AI/SymbolicPlanning.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AITaskScheduler.h"
#include "Horo/AI/BehaviorTreeRuntime.h"

#include <algorithm>
#include <new>
#include <tuple>

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

        std::optional<SymbolicPlanStatus> Expand(const std::size_t current, const std::span<const Action> actions, AiWorkBudget &budget,
                                                 const CancellationToken &cancellation, const AiTaskOperationContext *operation) noexcept {
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
            if (!source.action.IsValid() || source.cost == 0 || !task || (task->blackboard && task->blackboard != blackboard) || !before ||
                !after || after->known == 0)
                return Domain::Failure(MakeError(AIErrors::TaskContextInvalid));
            result.actions_[result.actionCount_++] = {{source.action, source.task}, source.cost, *before, *after};
        }
        const auto identity = [](const Action &action) {
            return action.identity.action;
        };
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

    SymbolicPlan SymbolicDomain::SearchImpl(const std::span<const SymbolicLiteral> initial, const std::span<const SymbolicLiteral> goal,
                                            const std::size_t work, const CancellationToken &cancellation,
                                            const AiTaskOperationContext *operation) const noexcept {
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

    const SymbolicDomain::Action *SymbolicHierarchy::FindAction(const SymbolicActionId id) const noexcept {
        const auto actions = std::span{primitives_->actions_}.first(primitives_->actionCount_);
        const auto identity = [](const SymbolicDomain::Action &action) {
            return action.identity.action;
        };
        const auto found = std::ranges::lower_bound(actions, id, {}, identity);
        return found != actions.end() && found->identity.action == id ? &*found : nullptr;
    }

    bool SymbolicHierarchy::HasCompound(const SymbolicCompoundId compound) const noexcept {
        const auto methods = std::span{methods_}.first(methodCount_);
        return std::ranges::binary_search(methods, compound, {}, &Method::compound);
    }

    Result<SymbolicHierarchy> SymbolicHierarchy::Capture(std::shared_ptr<const SymbolicDomain> primitives,
                                                         const std::span<const SymbolicMethodSource> sources) {
        using Hierarchy = Result<SymbolicHierarchy>;
        if (!primitives || sources.empty() || sources.size() > MaximumMethods)
            return Hierarchy::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        SymbolicHierarchy hierarchy;
        hierarchy.primitives_ = std::move(primitives);
        for (const auto &source : sources) {
            const auto conditions = hierarchy.primitives_->Encode(source.preconditions);
            if (!source.method.IsValid() || !source.compound.IsValid() || !conditions || source.children.size() > MaximumChildren)
                return Hierarchy::Failure(MakeError(AIErrors::TaskContextInvalid));
            auto &method = hierarchy.methods_[hierarchy.methodCount_++];
            method.id = source.method;
            method.compound = source.compound;
            method.preconditions = *conditions;
            method.childCount = source.children.size();
            std::ranges::copy(source.children, method.children.begin());
        }
        auto methods = std::span{hierarchy.methods_}.first(hierarchy.methodCount_);
        std::ranges::sort(methods, {}, &Method::id);
        if (std::ranges::adjacent_find(methods, {}, &Method::id) != methods.end())
            return Hierarchy::Failure(MakeError(AIErrors::IdentityInvalid));
        std::ranges::sort(methods, [](const Method &left, const Method &right) {
            return std::tie(left.compound, left.id) < std::tie(right.compound, right.id);
        });
        const auto valid = [&](const SymbolicSubtask &child) {
            if (const auto *action = std::get_if<SymbolicActionId>(&child))
                return hierarchy.FindAction(*action) != nullptr;
            return hierarchy.HasCompound(std::get<SymbolicCompoundId>(child));
        };
        for (const auto &method : methods) {
            if (!std::ranges::all_of(std::span{method.children}.first(method.childCount), valid))
                return Hierarchy::Failure(MakeError(AIErrors::TaskContextInvalid));
        }
        return Hierarchy::Success(std::move(hierarchy));
    }

    struct SymbolicHierarchy::Decomposition final {
        static_assert(MaximumDepth < SymbolicPlan::MaximumStates);

        /** @brief Structural bounds apply to one branch. Work exhaustion ends the whole search. */
        enum class Outcome : std::uint8_t {
            Solved,
            NoPlan,
            BranchBound,
            WorkExhausted,
            Cancelled
        };

        struct Pending final {
            std::array<SymbolicSubtask, MaximumDepth> tasks{};
            std::size_t count{};
        };

        const SymbolicHierarchy &hierarchy;
        AiWorkBudget &budget;
        const CancellationToken &cancellation;
        const AiTaskOperationContext *operation;
        SymbolicPlan &plan;

        static SymbolicPlanStatus ResultStatus(const Outcome outcome) noexcept {
            switch (outcome) {
                case Outcome::Solved:
                    return SymbolicPlanStatus::Solved;
                case Outcome::NoPlan:
                    return SymbolicPlanStatus::NoPlan;
                case Outcome::BranchBound:
                case Outcome::WorkExhausted:
                    return SymbolicPlanStatus::BudgetExhausted;
                case Outcome::Cancelled:
                    return SymbolicPlanStatus::Cancelled;
            }
            return SymbolicPlanStatus::InvalidInput;
        }

        Outcome Solve(Pending pending, const std::uint64_t state, const std::size_t depth) noexcept {
            using enum Outcome;
            if (PlanningCancelled(cancellation, operation))
                return Cancelled;
            if (pending.count == 0)
                return Solved;
            if (depth == MaximumDepth)
                return BranchBound;
            if (!budget.Consume(1))
                return WorkExhausted;
            const auto &next = pending.tasks[--pending.count];
            if (const auto *action = std::get_if<SymbolicActionId>(&next))
                return Primitive(*action, pending, state, depth + 1);
            return Compound(std::get<SymbolicCompoundId>(next), pending, state, depth + 1);
        }

        Outcome Primitive(SymbolicActionId id, const Pending &pending, std::uint64_t state, const std::size_t depth) noexcept {
            using enum Outcome;
            const auto *action = hierarchy.FindAction(id);
            if (!action || !action->preconditions.Matches(state))
                return NoPlan;
            state = (state & ~action->effects.known) | action->effects.values;
            plan.steps_[plan.count_++] = action->identity;
            plan.cost_ += action->cost;
            const auto outcome = Solve(pending, state, depth);
            if (outcome != Solved) {
                --plan.count_;
                plan.cost_ -= action->cost;
            }
            return outcome;
        }

        /** @brief Expands one admitted method while preserving the caller's pending stack for backtracking. */
        Outcome ExpandMethod(const Method &method, const Pending &pending, const std::uint64_t state, const std::size_t depth) noexcept {
            if (method.childCount > pending.tasks.size() - pending.count)
                return Outcome::BranchBound;
            if (!budget.Consume(method.childCount))
                return Outcome::WorkExhausted;
            auto expanded = pending;
            for (auto index = method.childCount; index > 0; --index)
                expanded.tasks[expanded.count++] = method.children[index - 1];
            return Solve(expanded, state, depth);
        }

        Outcome Compound(SymbolicCompoundId compound, const Pending &pending, const std::uint64_t state,
                         const std::size_t depth) noexcept {
            using enum Outcome;
            const auto methods = std::span{hierarchy.methods_}.first(hierarchy.methodCount_);
            const auto first = std::ranges::lower_bound(methods, compound, {}, &Method::compound);
            auto incomplete = NoPlan;
            for (auto method = first; method != methods.end() && method->compound == compound; ++method) {
                if (PlanningCancelled(cancellation, operation))
                    return Cancelled;
                if (!budget.Consume(1))
                    return WorkExhausted;
                if (!method->preconditions.Matches(state))
                    continue;
                const auto outcome = ExpandMethod(*method, pending, state, depth);
                if (outcome == BranchBound)
                    incomplete = BranchBound;
                else if (outcome != NoPlan)
                    return outcome;
            }
            return incomplete;
        }
    };

    SymbolicPlan SymbolicHierarchy::Search(const std::span<const SymbolicLiteral> initial, const SymbolicCompoundId root,
                                           const std::size_t work, const CancellationToken &cancellation) const noexcept {
        return SearchImpl(initial, root, work, cancellation, nullptr);
    }

    SymbolicPlan SymbolicHierarchy::SearchImpl(const std::span<const SymbolicLiteral> initial, const SymbolicCompoundId root,
                                               const std::size_t work, const CancellationToken &cancellation,
                                               const AiTaskOperationContext *operation) const noexcept {
        using enum SymbolicPlanStatus;
        SymbolicPlan plan;
        if (!primitives_)
            return plan;
        const auto facts = primitives_->Encode(initial);
        if (!facts || !HasCompound(root) || work == 0 || work > SymbolicDomain::MaximumWork)
            return plan;
        AiWorkBudget budget{work};
        Decomposition search{*this, budget, cancellation, operation, plan};
        const auto outcome = search.Solve({{root}, 1}, facts->values, 0);
        plan.status_ = PlanningCancelled(cancellation, operation) ? Cancelled : Decomposition::ResultStatus(outcome);
        if (plan.status_ != Solved) {
            plan.count_ = 0;
            plan.cost_ = 0;
        }
        return plan;
    }

    Result<std::shared_ptr<const BehaviorTreeExecutionPlan>> CompileSymbolicTaskSequence(const SymbolicPlan &plan,
                                                                                         SymbolicExecutionBindings bindings,
                                                                                         const AiCapabilityDescriptorSet &tasks) {
        using Compiled = Result<std::shared_ptr<const BehaviorTreeExecutionPlan>>;
        const auto steps = plan.Steps();
        if (plan.Status() != SymbolicPlanStatus::Solved || steps.size() != bindings.occurrences.size())
            return Compiled::Failure(MakeError(AIErrors::TaskContextInvalid));
        if (steps.empty())
            return Compiled::Success(nullptr);
        if (!bindings.asset || !bindings.sequence.IsValid())
            return Compiled::Failure(MakeError(AIErrors::DecisionAssetBindingMissing));
        const auto schema = bindings.asset->BlackboardSchema();
        if (!schema)
            return Compiled::Failure(MakeError(AIErrors::DecisionAssetBindingMissing));

        try {
            BehaviorTreeExecutionNode root{.id = bindings.sequence, .operation = BehaviorTreeOperation::Sequence};
            root.children.reserve(steps.size());
            std::vector<BehaviorTreeExecutionNode> nodes;
            nodes.reserve(steps.size() + 1);
            for (std::size_t index = 0; index < steps.size(); ++index) {
                const auto &step = steps[index];
                const auto &occurrence = bindings.occurrences[index];
                const auto task = tasks.Find(step.task);
                if (!task || (task->blackboard && task->blackboard != schema->Identity()) || !occurrence.node.IsValid() ||
                    occurrence.action != step.action || occurrence.task != step.task)
                    return Compiled::Failure(MakeError(AIErrors::TaskContextInvalid));
                root.children.push_back(occurrence.node);
                nodes.push_back({.id = occurrence.node, .operation = BehaviorTreeOperation::Task, .task = step.task});
            }
            nodes.push_back(std::move(root));
            return BehaviorTreeExecutionPlan::Compile(std::move(bindings.asset), bindings.sequence, nodes, {},
                                                      {.maximumNodes = nodes.size(), .maximumDepth = 2});
        } catch (const std::bad_alloc &) {
            return Compiled::Failure(MakeError(AIErrors::BehaviorTreeStorageUnavailable));
        }
    }

    Result<std::shared_ptr<SymbolicPlanningJob>> SymbolicPlanningJob::CaptureInputs(const std::span<const SymbolicLiteral> initial,
                                                                                    const std::size_t work) {
        using Job = Result<std::shared_ptr<SymbolicPlanningJob>>;
        if (initial.size() > SymbolicDomain::MaximumFacts || work == 0 || work > SymbolicDomain::MaximumWork)
            return Job::Failure(MakeError(AIErrors::TaskContextInvalid));
        try {
            auto job = std::shared_ptr<SymbolicPlanningJob>{new SymbolicPlanningJob};
            std::ranges::copy(initial, job->initial_.begin());
            job->initialCount_ = initial.size();
            job->work_ = work;
            return Job::Success(std::move(job));
        } catch (const std::bad_alloc &) {
            return Job::Failure(MakeError(AIErrors::AgentCapacityExceeded));
        }
    }

    Result<std::shared_ptr<SymbolicPlanningJob>> SymbolicPlanningJob::Create(std::shared_ptr<const SymbolicDomain> domain,
                                                                             const std::span<const SymbolicLiteral> initial,
                                                                             const std::span<const SymbolicLiteral> goal,
                                                                             const std::size_t work) {
        if (!domain || !domain->Encode(initial) || !domain->Encode(goal))
            return Result<std::shared_ptr<SymbolicPlanningJob>>::Failure(MakeError(AIErrors::TaskContextInvalid));
        auto job = CaptureInputs(initial, work);
        if (job.HasValue()) {
            job.Value()->domain_ = std::move(domain);
            std::ranges::copy(goal, job.Value()->goal_.begin());
            job.Value()->goalCount_ = goal.size();
        }
        return job;
    }

    Result<std::shared_ptr<SymbolicPlanningJob>> SymbolicPlanningJob::Create(std::shared_ptr<const SymbolicHierarchy> hierarchy,
                                                                             const std::span<const SymbolicLiteral> initial,
                                                                             const SymbolicCompoundId root, const std::size_t work) {
        if (!hierarchy || !hierarchy->primitives_ || !hierarchy->primitives_->Encode(initial) || !hierarchy->HasCompound(root))
            return Result<std::shared_ptr<SymbolicPlanningJob>>::Failure(MakeError(AIErrors::TaskContextInvalid));
        auto job = CaptureInputs(initial, work);
        if (job.HasValue()) {
            job.Value()->hierarchy_ = std::move(hierarchy);
            job.Value()->root_ = root;
        }
        return job;
    }

    Result<void> SymbolicPlanningJob::Run(const AiTaskOperationContext &operation, const CancellationToken &cancellation) const {
        if (!operation.taskDefinition.IsValid() || !operation.task.IsValid() || !operation.agent.IsValid() ||
            operation.task.incarnation != operation.agent.incarnation)
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        auto expected = State::Ready;
        if (!state_.compare_exchange_strong(expected, State::Running))
            return Result<void>::Failure(MakeError(AIErrors::TaskTransitionInvalid));
        resultCancellation_ = {cancellation, operation.cancellation, operation.capabilityCancellation, operation.ownerCancellation};
        const auto initial = std::span{initial_}.first(initialCount_);
        if (hierarchy_)
            result_ = hierarchy_->SearchImpl(initial, root_, work_, cancellation, &operation);
        else
            result_ = domain_->SearchImpl(initial, std::span{goal_}.first(goalCount_), work_, cancellation, &operation);
        const bool cancelled = result_.Status() == SymbolicPlanStatus::Cancelled || PlanningCancelled(cancellation, &operation);
        state_.store(State::Complete, std::memory_order_release);
        return cancelled ? JobCancelled() : Result<void>::Success();
    }

    const SymbolicPlan *SymbolicPlanningJob::Read() const noexcept {
        if (state_.load(std::memory_order_acquire) != State::Complete)
            return nullptr;
        if (result_.Status() == SymbolicPlanStatus::Solved &&
            std::ranges::any_of(resultCancellation_, &CancellationToken::IsCancellationRequested))
            return nullptr;
        return &result_;
    }
}  // namespace Horo::AI
