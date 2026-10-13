#include "AnimationStateMachineInternal.h"

#include <cmath>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Proves typed threshold access before a condition is retained by a compiled graph. */
        GraphInstanceFailure ValidateCondition(const StateMachineCondition &condition,
                                               const std::span<const GraphParameter> parameters) noexcept {
            using enum GraphInstanceFailure;
            if (condition.parameter >= parameters.size())
                return BindingMismatch;
            const auto &parameter = parameters[condition.parameter];
            if (condition.predicate > StateMachinePredicate::Trigger)
                return Malformed;
            if (condition.predicate == StateMachinePredicate::Above || condition.predicate == StateMachinePredicate::Below) {
                const auto *threshold = std::get_if<float>(&condition.threshold);
                if (parameter.type != GraphValueType::Float || !threshold)
                    return TypeMismatch;
                if (!std::isfinite(*threshold) || !std::isfinite(condition.releaseThreshold))
                    return Malformed;
                if ((condition.predicate == StateMachinePredicate::Above && condition.releaseThreshold > *threshold) ||
                    (condition.predicate == StateMachinePredicate::Below && condition.releaseThreshold < *threshold))
                    return Malformed;
                return None;
            }
            if (condition.predicate == StateMachinePredicate::Trigger)
                return parameter.type == GraphValueType::Trigger && std::get_if<bool>(&condition.threshold) &&
                               std::get<bool>(condition.threshold)
                           ? None
                           : TypeMismatch;
            if (parameter.type != GraphValueType::Integer && parameter.type != GraphValueType::Boolean)
                return TypeMismatch;
            return parameter.defaultValue.index() == condition.threshold.index() ? None : TypeMismatch;
        }
    }  // namespace

    /** @copydoc ValidateStateMachine */
    GraphInstanceFailure ValidateStateMachine(const StateMachinePlan &plan, const std::span<const GraphParameter> parameters) noexcept {
        using enum GraphInstanceFailure;
        if (plan.states.empty() || plan.states.size() > StateMachinePlan::MaximumStates ||
            plan.transitions.size() > StateMachinePlan::MaximumTransitions || plan.conditions.size() > StateMachinePlan::MaximumConditions)
            return LimitExceeded;
        if (plan.initial >= plan.states.size())
            return BindingMismatch;
        for (std::size_t index = 0; index < plan.states.size(); ++index) {
            if (plan.states[index].id.value == 0)
                return Malformed;
            for (std::size_t earlier = 0; earlier < index; ++earlier)
                if (plan.states[earlier].id == plan.states[index].id)
                    return Malformed;
        }
        std::size_t nextCondition = 0;
        for (std::size_t index = 0; index < plan.transitions.size(); ++index) {
            const auto &transition = plan.transitions[index];
            if (transition.source >= plan.states.size() || transition.destination >= plan.states.size())
                return BindingMismatch;
            if (transition.id.value == 0 || transition.duration.ticks < 0 || (transition.exitTime && transition.exitTime->ticks < 0) ||
                transition.interruption > StateMachineInterruption::Either || transition.curve > StateMachineBlendCurve::EaseOut)
                return Malformed;
            for (std::size_t earlier = 0; earlier < index; ++earlier)
                if (plan.transitions[earlier].id == transition.id)
                    return Malformed;
            if (transition.firstCondition != nextCondition || transition.conditions > plan.conditions.size() - nextCondition)
                return Malformed;
            nextCondition += transition.conditions;
        }
        if (nextCondition != plan.conditions.size())
            return Malformed;
        for (const auto &condition : plan.conditions)
            if (const auto failure = ValidateCondition(condition, parameters); failure != None)
                return failure;
        return None;
    }
}  // namespace Horo::Animation::Detail
