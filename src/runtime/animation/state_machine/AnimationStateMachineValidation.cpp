#include "AnimationStateMachineInternal.h"

#include <cmath>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Validates ordered hysteresis thresholds only after proving their scalar type. */
        GraphInstanceFailure ValidateFloatCondition(const StateMachineCondition &condition, const GraphParameter &parameter) noexcept {
            using enum GraphInstanceFailure;
            const auto *threshold = std::get_if<float>(&condition.threshold);
            if (parameter.type != GraphValueType::Float || !threshold)
                return TypeMismatch;
            if (!std::isfinite(*threshold) || !std::isfinite(condition.releaseThreshold))
                return Malformed;
            if (condition.predicate == StateMachinePredicate::Above)
                return condition.releaseThreshold > *threshold ? Malformed : None;
            return condition.releaseThreshold < *threshold ? Malformed : None;
        }

        /** @brief Trigger conditions admit only the typed asserted trigger operand. */
        GraphInstanceFailure ValidateTriggerCondition(const StateMachineCondition &condition, const GraphParameter &parameter) noexcept {
            const auto *threshold = std::get_if<bool>(&condition.threshold);
            return parameter.type == GraphValueType::Trigger && threshold && *threshold ? GraphInstanceFailure::None
                                                                                        : GraphInstanceFailure::TypeMismatch;
        }

        /** @brief Proves typed threshold access before a condition is retained by a compiled graph. */
        GraphInstanceFailure ValidateCondition(const StateMachineCondition &condition,
                                               const std::span<const GraphParameter> parameters) noexcept {
            using enum GraphInstanceFailure;
            if (condition.parameter >= parameters.size())
                return BindingMismatch;
            const auto &parameter = parameters[condition.parameter];
            if (condition.predicate > StateMachinePredicate::Trigger)
                return Malformed;
            if (condition.predicate == StateMachinePredicate::Above || condition.predicate == StateMachinePredicate::Below)
                return ValidateFloatCondition(condition, parameter);
            if (condition.predicate == StateMachinePredicate::Trigger)
                return ValidateTriggerCondition(condition, parameter);
            if (parameter.type != GraphValueType::Integer && parameter.type != GraphValueType::Boolean)
                return TypeMismatch;
            return parameter.defaultValue.index() == condition.threshold.index() ? None : TypeMismatch;
        }

        /** @brief Admits nonzero unique state identities without allocating a temporary index. */
        GraphInstanceFailure ValidateStates(const StateMachinePlan &plan) noexcept {
            using enum GraphInstanceFailure;
            for (std::size_t index = 0; index < plan.states.size(); ++index) {
                if (plan.states[index].id.value == 0)
                    return Malformed;
                for (std::size_t earlier = 0; earlier < index; ++earlier)
                    if (plan.states[earlier].id == plan.states[index].id)
                        return Malformed;
            }
            return None;
        }

        /** @brief Admits only supported nonnegative transition policy operands. */
        bool ValidTransitionPolicy(const StateMachineTransition &transition) noexcept {
            return transition.id.value != 0 && transition.duration.ticks >= 0 &&
                   (!transition.exitTime || transition.exitTime->ticks >= 0) &&
                   transition.interruption <= StateMachineInterruption::Either && transition.curve <= StateMachineBlendCurve::EaseOut;
        }

        /** @brief Retained transition identity is unique among earlier declarations. */
        bool UniqueTransition(const StateMachinePlan &plan, const std::size_t index) noexcept {
            for (std::size_t earlier = 0; earlier < index; ++earlier)
                if (plan.transitions[earlier].id == plan.transitions[index].id)
                    return false;
            return true;
        }

        /** @brief Checks one transition in binding, policy, identity, then condition-range order. */
        GraphInstanceFailure ValidateTransition(const StateMachinePlan &plan, const std::size_t index,
                                                const std::size_t nextCondition) noexcept {
            using enum GraphInstanceFailure;
            const auto &transition = plan.transitions[index];
            if (transition.source >= plan.states.size() || transition.destination >= plan.states.size())
                return BindingMismatch;
            if (!ValidTransitionPolicy(transition))
                return Malformed;
            if (!UniqueTransition(plan, index))
                return Malformed;
            if (transition.firstCondition != nextCondition || transition.conditions > plan.conditions.size() - nextCondition)
                return Malformed;
            return None;
        }

        /** @brief Walks the exact contiguous condition partition, admitting every range before advancing it. */
        GraphInstanceFailure ValidateTransitions(const StateMachinePlan &plan) noexcept {
            std::size_t nextCondition = 0;
            for (std::size_t index = 0; index < plan.transitions.size(); ++index) {
                if (const auto failure = ValidateTransition(plan, index, nextCondition); failure != GraphInstanceFailure::None)
                    return failure;
                nextCondition += plan.transitions[index].conditions;
            }
            return nextCondition == plan.conditions.size() ? GraphInstanceFailure::None : GraphInstanceFailure::Malformed;
        }
    }  // namespace

    /** @copydoc StateMachineStorageAdmitted */
    bool StateMachineStorageAdmitted(const StateMachinePlan &plan) noexcept {
        return !plan.states.empty() && plan.states.size() <= StateMachinePlan::MaximumStates &&
               plan.transitions.size() <= StateMachinePlan::MaximumTransitions &&
               plan.conditions.size() <= StateMachinePlan::MaximumConditions;
    }

    /** @copydoc ValidateStateMachine */
    GraphInstanceFailure ValidateStateMachine(const StateMachinePlan &plan, const std::span<const GraphParameter> parameters) noexcept {
        using enum GraphInstanceFailure;
        if (!StateMachineStorageAdmitted(plan))
            return LimitExceeded;
        if (plan.initial >= plan.states.size())
            return BindingMismatch;
        if (const auto failure = ValidateStates(plan); failure != None)
            return failure;
        if (const auto failure = ValidateTransitions(plan); failure != None)
            return failure;
        for (const auto &condition : plan.conditions)
            if (const auto failure = ValidateCondition(condition, parameters); failure != None)
                return failure;
        return None;
    }
}  // namespace Horo::Animation::Detail
