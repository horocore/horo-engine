#include "DecisionInstanceState.h"

#include <algorithm>
#include <limits>

namespace Horo::AI {
    namespace {
        template <typename T> Result<T> Invalid(const ErrorCodeDescriptor &code = AIErrors::TaskContextInvalid) {
            return Result<T>::Failure(MakeError(code));
        }

        const StateMachineProviderAction *ProviderAction(const StateMachineState &state, StateMachineActionPhase phase) {
            switch (phase) {
                using enum Horo::AI::StateMachineActionPhase;
                case Entry:
                    return state.entryProvider ? &*state.entryProvider : nullptr;
                case Update:
                    return state.updateProvider ? &*state.updateProvider : nullptr;
                case Exit:
                    return state.exitProvider ? &*state.exitProvider : nullptr;
            }
            return nullptr;
        }

        Result<void> PublishAction(AiTaskLifecycle &lifecycle, AgentHandle agent, const Result<AiTaskState> &result) {
            if (result.HasError()) {
                auto failure = lifecycle.CompleteFailure(agent, {AiTaskFailureKind::Execution, result.ErrorValue()});
                return failure.HasError() ? Result<void>::Failure(failure.ErrorValue()) : Result<void>::Failure(result.ErrorValue());
            }
            if (result.Value() == AiTaskState::Succeeded) {
                auto success = lifecycle.CompleteSuccess(agent);
                return success.HasError() ? Result<void>::Failure(success.ErrorValue()) : Result<void>::Success();
            }
            if (result.Value() == AiTaskState::Failed) {
                auto failure = lifecycle.CompleteFailure(agent, {AiTaskFailureKind::Execution, MakeError(AIErrors::TaskFailureInvalid)});
                if (failure.HasError())
                    return Result<void>::Failure(failure.ErrorValue());
            } else {
                auto cancelled = lifecycle.RequestCancellation(AiTaskCancellationReason::Requested);
                if (cancelled.HasError())
                    return Result<void>::Failure(cancelled.ErrorValue());
            }
            return Invalid<void>(AIErrors::TaskFailureInvalid);
        }
    }  // namespace

    Result<void> DecisionGraphInstance::ActionAdapter::Prepare() {
        const auto &graph = owner.plan_->Graphs()[owner.plan_->Occurrences()[index].plan];
        const auto providerCount = std::ranges::count_if(graph.bindings->Nodes(), [](const auto &node) {
            return node.intrinsic == DecisionControlKind::Unspecified;
        });
        actions.reserve(static_cast<std::size_t>(providerCount));
        std::uint32_t slot{1};  // The first HSM slot belongs to its actual authored runner definition.
        for (const auto &node : graph.bindings->Nodes()) {
            if (node.intrinsic != DecisionControlKind::Unspecified)
                continue;
            auto action = std::make_unique<Action>();
            action->node = node.id;
            action->slot = slot++;
            action->generation = owner.binding_.firstTaskGeneration;
            actions.push_back(std::move(action));
        }
        return Result<void>::Success();
    }

    Result<void> DecisionGraphInstance::ActionAdapter::Invoke(const StateMachineActionInvocation &invocation) noexcept {
        if (!owner.Available())
            return Invalid<void>(AIErrors::RuntimeUnavailable);
        const auto &graph = owner.plan_->Graphs()[owner.plan_->Occurrences()[index].plan];
        const auto &machine = *std::get<std::shared_ptr<const CookedStateMachinePlan>>(graph.execution);
        const auto state = std::ranges::find(machine.States(), invocation.state, &StateMachineState::id);
        if (state == machine.States().end())
            return Invalid<void>();
        const auto *definition = ProviderAction(*state, invocation.phase);
        if (!definition)
            return Invalid<void>(AIErrors::DecisionAssetDescriptorMissing);
        const auto found = std::ranges::find_if(actions, [&](const auto &action) {
            return action->node == definition->binding.id;
        });
        if (found == actions.end())
            return Invalid<void>();
        auto &action = **found;
        if (action.generation == 0)
            return Invalid<void>(AIErrors::GenerationExhausted);
        auto operation = owner.Operation(index, invocation.action, action.slot, action.generation);
        action.generation = action.generation == std::numeric_limits<std::uint32_t>::max() ? 0 : action.generation + 1;
        std::destroy_at(&action.lifecycle);
        std::construct_at(&action.lifecycle);
        if (auto started = action.lifecycle.Start(operation); started.HasError())
            return Result<void>::Failure(started.ErrorValue());
        if (action.lifecycle.State() != AiTaskState::Running)
            return Invalid<void>(AIErrors::TaskTransitionInvalid);
        BehaviorTreeEvaluationContext context{action.node, graph.bindings->BindingsForNode(action.node), invocation.blackboard,
                                              invocation.tick};
        return Complete(action, provider->Start(context, operation));
    }

    Result<void> DecisionGraphInstance::ActionAdapter::Complete(Action &action, Result<AiTaskState> result) {
        auto published = PublishAction(action.lifecycle, owner.binding_.operation.agent, result);
        const auto *terminal = action.lifecycle.TerminalResult();
        if (!terminal)
            return Invalid<void>(AIErrors::TaskTransitionInvalid);
        auto claimed = action.lifecycle.ClaimCleanup();
        if (claimed.HasError())
            return Result<void>::Failure(claimed.ErrorValue());
        if (!claimed.Value())
            return Invalid<void>(AIErrors::TaskTransitionInvalid);
        const auto *operation = action.lifecycle.Context();
        if (terminal->cancellationReason)
            provider->Abort(*operation, *terminal->cancellationReason);
        provider->Cleanup(*operation, *terminal);
        if (auto cleaned = action.lifecycle.CompleteCleanup(); cleaned.HasError())
            return Result<void>::Failure(cleaned.ErrorValue());
        if (terminal->state != AiTaskState::Succeeded && published.HasValue())
            return Invalid<void>(AIErrors::TaskFailureInvalid);
        return published;
    }

    Result<void> DecisionGraphInstance::ActionAdapter::EnterState(const StateMachineStateInvocation &invocation) noexcept {
        return owner.BeginCall(index, invocation.state);
    }

    Result<AiTaskState> DecisionGraphInstance::ActionAdapter::UpdateState(const StateMachineStateInvocation &invocation) noexcept {
        return owner.Call(index, invocation.state, false);
    }

    Result<void> DecisionGraphInstance::ActionAdapter::ExitState(const StateMachineStateInvocation &invocation) noexcept {
        owner.EndCall(index, invocation.state, AiTaskCancellationReason::Superseded);
        return Result<void>::Success();
    }

    void DecisionGraphInstance::ActionAdapter::CancelState(DecisionNodeId node, AiTaskCancellationReason reason) noexcept {
        owner.EndCall(index, node, reason);
    }

    DecisionGraphInstance::OccurrenceState::~OccurrenceState() = default;
}  // namespace Horo::AI
