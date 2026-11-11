#include "DecisionInstanceState.h"

#include <algorithm>

namespace Horo::AI {
    /** @copydoc DecisionGraphInstance::BuildUtility */
    Result<void> DecisionGraphInstance::BuildUtility(const std::size_t index) {
        const auto &graph = plan_->Graphs()[plan_->Occurrences()[index].plan];
        const auto &utility = *std::get<std::shared_ptr<const CookedUtilityPlan>>(graph.execution);
        auto &state = *occurrences_[index];
        state.utility = std::make_unique<UtilityAdapter>();
        state.utility->actions.reserve(utility.Actions().size());
        std::uint32_t offset{};
        for (const auto &action : utility.Actions()) {
            auto callbacks = providers_->CreateExecutor(providers_, *action, plan_->Capabilities(), graph.providerProperties);
            if (callbacks.HasError())
                return Result<void>::Failure(callbacks.ErrorValue());
            BehaviorTreeInstanceBinding binding{binding_.operation.agent, binding_.operation.cancellation,
                                                binding_.firstTaskSlot +
                                                    static_cast<std::uint32_t>(plan_->Occurrences()[index].firstTaskSlot) + offset,
                                                binding_.firstTaskGeneration, binding_.blackboard};
            binding.capabilityCancellation = binding_.operation.capabilityCancellation;
            binding.diagnostics = binding_.operation.diagnostics;
            auto runner = BehaviorTreeInstance::Create(action, binding, std::move(callbacks).Value());
            if (runner.HasError())
                return Result<void>::Failure(runner.ErrorValue());
            state.utility->actions.push_back(std::move(runner).Value());
            offset += static_cast<std::uint32_t>(action->TaskSlots());
        }
        return Result<void>::Success();
    }

    /** @copydoc DecisionGraphInstance::AdvanceUtility */
    Result<AiTaskState> DecisionGraphInstance::AdvanceUtility(const std::size_t index) {
        auto &state = *occurrences_[index];
        auto &runtime = *state.utility;
        const auto &utility =
            *std::get<std::shared_ptr<const CookedUtilityPlan>>(plan_->Graphs()[plan_->Occurrences()[index].plan].execution);
        if (!runtime.selected) {
            auto evaluated = utility.Selector().Evaluate(input_->blackboard, input_->current.agent, *input_->utilityAgentBudget,
                                                         *input_->utilityGlobalBudget, runtime.evidence);
            if (evaluated.HasError())
                return Result<AiTaskState>::Failure(evaluated.ErrorValue());
            const auto &result = evaluated.Value();
            if (trace_) {
                for (const auto &record : std::span{runtime.evidence}.first(result.evidenceWritten))
                    trace_->Append({.kind = DecisionStepRecordKind::UtilityScore,
                                    .occurrence = TraceOccurrence(index),
                                    .node = record.option,
                                    .key = record.key,
                                    .utilityScore = record.score});
                trace_->dropped_ += result.evidenceDropped;
            }
            if (result.budgetExhausted) {
                state.outcome = AiTaskState::Running;
                return Result<AiTaskState>::Success(state.outcome);
            }
            if (!result.selection) {
                state.outcome = AiTaskState::Failed;
                return Result<AiTaskState>::Success(state.outcome);
            }
            const auto options = utility.Selector().Options();
            const auto selected = std::ranges::find(options, result.selection->option, &UtilityOption::id);
            runtime.selected = static_cast<std::size_t>(selected - options.begin());
        }
        auto &action = *runtime.actions[*runtime.selected];
        auto stepped = action.Evaluate(input_->tick, input_->blackboard, input_->current.agent);
        if (trace_)
            action.AppendTrace(TraceOccurrence(index), *trace_);
        if (stepped.HasError())
            return stepped;
        state.outcome = stepped.Value();
        return stepped;
    }
}  // namespace Horo::AI
