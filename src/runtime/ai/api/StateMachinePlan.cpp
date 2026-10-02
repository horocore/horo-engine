#include "Horo/AI/StateMachine.h"

#include <algorithm>
#include <new>
#include <ranges>

namespace Horo::AI {
    namespace StateMachineErrors {
        const ErrorCodeDescriptor TopologyInvalid{
            .domain = ErrorDomainId{"horo.ai"},
            .code = ErrorCode{"ai.state_machine.topology_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "State-machine nodes, transition endpoints or initial children are inconsistent.",
            .remediationHint = "Use unique admitted nodes, existing endpoints and one explicit initial child for every compound state.",
            .userActionable = true,
        };
        const ErrorCodeDescriptor HierarchyInvalid{
            .domain = ErrorDomainId{"horo.ai"},
            .code = ErrorCode{"ai.state_machine.hierarchy_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The state parent hierarchy contains a cycle or exceeds the admitted depth.",
            .remediationHint = "Remove parent cycles and limit each active ancestor path to sixteen states.",
            .userActionable = true,
        };
        const ErrorCodeDescriptor StepInvalid{
            .domain = ErrorDomainId{"horo.ai"},
            .code = ErrorCode{"ai.state_machine.step_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "A state-machine step is repeated, reentrant, malformed, oversized or cancelled.",
            .remediationHint = "Evaluate once per increasing owner tick with bounded typed events and a current admitted lifecycle.",
            .userActionable = true,
        };
    }  // namespace StateMachineErrors

    namespace {
        /** @brief Reports a typed admission failure without publishing a partial plan. */
        template <typename T> Result<T> Invalid(const ErrorCodeDescriptor &code = AIErrors::DecisionAssetSchemaInvalid) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Finds a state in the canonical identity-sorted source. */
        const StateMachineState *FindState(const StateMachineAssetDescriptor &source, const DecisionNodeId id) {
            const auto found = std::ranges::lower_bound(source.states, id, {}, &StateMachineState::id);
            return found != source.states.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        /** @brief Validates one compound state's explicit initial-child selection. */
        Result<void> ValidateInitialChild(const StateMachineAssetDescriptor &source, const StateMachineState &state) {
            const bool hasChildren = std::ranges::any_of(source.states, [&](const auto &child) {
                return child.parent == state.id;
            });
            if (hasChildren != state.initialChild.IsValid())
                return Invalid<void>(StateMachineErrors::TopologyInvalid);
            if (hasChildren) {
                const auto *child = FindState(source, state.initialChild);
                if (child == nullptr || child->parent != state.id)
                    return Invalid<void>(StateMachineErrors::TopologyInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Bounds one parent chain and rejects dangling parent identities before execution. */
        Result<void> ValidateAncestorPath(const StateMachineAssetDescriptor &source, const StateMachineState &state) {
            const auto *ancestor = &state;
            std::size_t depth{};
            while (ancestor != nullptr) {
                if (++depth > StateMachineLimits::Depth)
                    return Invalid<void>(StateMachineErrors::HierarchyInvalid);
                if (!ancestor->parent.IsValid())
                    break;
                ancestor = FindState(source, ancestor->parent);
                if (ancestor == nullptr)
                    return Invalid<void>(StateMachineErrors::TopologyInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Admits state identities and action definitions before validating bounded hierarchy paths. */
        Result<void> ValidateHierarchy(const StateMachineAssetDescriptor &source) {
            for (const auto &state : source.states) {
                if (!state.id.IsValid())
                    return Invalid<void>();
                for (const auto &action : {state.entry, state.update, state.exit}) {
                    if (action.has_value() && !action->IsValid())
                        return Invalid<void>();
                }
                for (const auto &valid : {ValidateInitialChild(source, state), ValidateAncestorPath(source, state)}) {
                    if (valid.HasError())
                        return valid;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Checks the semantic source's fixed activation and step-policy bounds. */
        Result<void> ValidateCapacity(const StateMachineAssetDescriptor &source) {
            if (source.states.empty())
                return Invalid<void>();
            if (source.states.size() > StateMachineLimits::States || source.transitions.size() > StateMachineLimits::Transitions ||
                source.maximumTransitionsPerStep == 0 || source.maximumTransitionsPerStep > StateMachineLimits::TransitionsPerStep)
                return Invalid<void>(AIErrors::DecisionAssetLimitExceeded);
            return Result<void>::Success();
        }

        /** @brief Validates a guard against its exact shared-plan key binding and schema value rules. */
        Result<void> ValidateGuard(const StateMachineGuard &guard, const std::span<const DecisionPlanBlackboardBinding> bindings,
                                   const BlackboardSchema &schema) {
            using enum StateMachineGuardOperator;
            if (guard.operation >= Count)
                return Invalid<void>();
            const auto found = std::ranges::find(bindings, guard.key, &DecisionPlanBlackboardBinding::key);
            if (found == bindings.end())
                return Invalid<void>(AIErrors::DecisionAssetBindingMissing);
            if (guard.operation == Present)
                return Result<void>::Success();
            if (found->cardinality != BlackboardValueCardinality::Scalar)
                return Invalid<void>(AIErrors::DecisionAssetBindingTypeMismatch);
            if (const bool ordered = guard.operation == Less || guard.operation == Greater;
                ordered && found->kind != BlackboardValueKind::SignedInteger && found->kind != BlackboardValueKind::Scalar)
                return Invalid<void>(AIErrors::DecisionAssetBindingTypeMismatch);
            return ValidateBlackboardValue(BlackboardValue{guard.operand}, &schema.Keys()[found->schemaIndex],
                                           BlackboardUnknownValuePolicy::Reject);
        }

        /** @brief Ensures state and transition identities partition the shared plan without competing node records. */
        Result<void> ValidateNodes(const StateMachineAssetDescriptor &source, const DecisionAssetPlan &decision) {
            if (FindState(source, source.initial) == nullptr)
                return Invalid<void>(StateMachineErrors::TopologyInvalid);
            if (decision.Nodes().size() != source.states.size() + source.transitions.size())
                return Invalid<void>(StateMachineErrors::TopologyInvalid);
            for (const auto &node : decision.Nodes()) {
                const bool state = FindState(source, node.id) != nullptr;
                const auto count = std::ranges::count(source.transitions, node.id, &StateMachineTransition::id);
                if (static_cast<std::size_t>(state) + static_cast<std::size_t>(count) != 1)
                    return Invalid<void>(AIErrors::DescriptorConflict);
            }
            return Result<void>::Success();
        }

        /** @brief Validates transition references and every declared typed guard. */
        Result<void> ValidateTransitions(const StateMachineAssetDescriptor &source, const DecisionAssetPlan &decision) {
            for (const auto &transition : source.transitions) {
                if (!transition.id.IsValid() || FindState(source, transition.source) == nullptr ||
                    FindState(source, transition.target) == nullptr || (transition.event && !transition.event->IsValid()))
                    return Invalid<void>(StateMachineErrors::TopologyInvalid);
                if (transition.guards.size() > StateMachineLimits::GuardsPerTransition)
                    return Invalid<void>(AIErrors::DecisionAssetLimitExceeded);
                for (const auto &guard : transition.guards) {
                    const auto valid = ValidateGuard(guard, decision.BindingsForNode(transition.id), *decision.BlackboardSchema());
                    if (valid.HasError())
                        return valid;
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CookedStateMachinePlan::Compile */
    Result<std::shared_ptr<const CookedStateMachinePlan>> CookedStateMachinePlan::Compile(std::shared_ptr<const DecisionAssetPlan> decision,
                                                                                          StateMachineAssetDescriptor source) {
        using Output = std::shared_ptr<const CookedStateMachinePlan>;
        if (decision == nullptr || decision->Kind() != DecisionPlanKind::StateMachine)
            return Invalid<Output>();
        if (const auto capacity = ValidateCapacity(source); capacity.HasError())
            return Result<Output>::Failure(capacity.ErrorValue());
        try {
            std::ranges::sort(source.states, {}, &StateMachineState::id);
            if (std::ranges::adjacent_find(source.states, {}, &StateMachineState::id) != source.states.end())
                return Invalid<Output>(AIErrors::DescriptorConflict);
            for (const auto &validation :
                 {ValidateHierarchy(source), ValidateNodes(source, *decision), ValidateTransitions(source, *decision)}) {
                if (validation.HasError())
                    return Result<Output>::Failure(validation.ErrorValue());
            }
            std::ranges::sort(source.transitions, [](const auto &left, const auto &right) {
                if (left.priority != right.priority)
                    return left.priority > right.priority;
                return left.id < right.id;
            });
            return Result<Output>::Success(
                std::make_shared<const CookedStateMachinePlan>(ConstructionToken{}, std::move(decision), std::move(source)));
        } catch (const std::bad_alloc &) {
            return Invalid<Output>(AIErrors::DecisionAssetStorageUnavailable);
        }
    }

    /** @brief Resolves one stable state identity to its admitted flat index. */
    std::size_t CookedStateMachinePlan::Index(const DecisionNodeId id) const noexcept {
        const auto found = std::ranges::lower_bound(source_.states, id, {}, &StateMachineState::id);
        return static_cast<std::size_t>(found - source_.states.begin());
    }
}  // namespace Horo::AI
