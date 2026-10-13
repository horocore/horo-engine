#include "AnimationGraphCompilerInternal.h"

#include <cmath>

namespace Horo::Animation::GraphCompileDetail {
    namespace {
        /** @brief Proves the finite float thresholds and ordered release bound for a hysteresis predicate. */
        bool ValidFloatCondition(const GraphStateCondition &condition, const GraphParameter &parameter) {
            const auto *threshold = std::get_if<float>(&condition.threshold);
            if (parameter.type != GraphValueType::Float || !threshold || !std::isfinite(*threshold))
                return false;
            return condition.predicate == GraphStatePredicate::Above ? condition.releaseThreshold <= *threshold
                                                                     : condition.releaseThreshold >= *threshold;
        }

        /** @brief Requires a trigger declaration and an explicitly true boolean threshold. */
        bool ValidTriggerCondition(const GraphStateCondition &condition, const GraphParameter &parameter) {
            const auto *value = std::get_if<bool>(&condition.threshold);
            return parameter.type == GraphValueType::Trigger && value && *value;
        }

        /** @brief Validates a typed predicate before the runtime may use its dense parameter access. */
        bool ValidCondition(const GraphStateCondition &condition, const GraphParameter &parameter) {
            if (condition.predicate > GraphStatePredicate::Trigger || !std::isfinite(condition.releaseThreshold))
                return false;
            using enum GraphStatePredicate;
            if (condition.predicate == Above || condition.predicate == Below)
                return ValidFloatCondition(condition, parameter);
            if (condition.predicate == Trigger)
                return ValidTriggerCondition(condition, parameter);
            return (parameter.type == GraphValueType::Boolean || parameter.type == GraphValueType::Integer) &&
                   condition.threshold.index() == parameter.defaultValue.index();
        }

        /** @brief Validates a state's sole source without conflating clip-derived clocks with authored subgraph cycles. */
        Result<void> ValidateSource(const GraphState &state, const AnimationGraphData &data, const GraphSourceLocation location) {
            if (const auto *clip = std::get_if<AnimationClipId>(&state.source)) {
                if (!clip->IsValid() || clip->Asset() == data.id.Asset() || clip->Asset() == data.skeleton.Asset() || state.loopPeriod)
                    return Result<void>::Failure(Failure(AnimationErrors::GraphBindingMismatch, location));
            } else {
                const auto *definition = Find(data.definitions, std::get<GraphDefinitionId>(state.source));
                if (!definition || !definition->inputs.empty())
                    return Result<void>::Failure(Failure(AnimationErrors::GraphBindingMismatch, location));
                if (state.loopPeriod && state.loopPeriod->ticks <= 0)
                    return Result<void>::Failure(Failure(AnimationErrors::GraphMalformed, location));
            }
            return Result<void>::Success();
        }

        /** @brief Validates ordered node-local state identities and their typed sources. */
        Result<void> ValidateStates(const GraphStateMachineNode &machine, const AnimationGraphData &data, GraphSourceLocation location) {
            for (std::size_t index = 0; index < machine.states.size(); ++index) {
                const auto &state = machine.states[index];
                if (!state.id.IsValid() || (index != 0 && state.id == machine.states[index - 1].id))
                    return Result<void>::Failure(Failure(AnimationErrors::GraphMalformed, location));
                if (auto checked = ValidateSource(state, data, location); checked.HasError())
                    return checked;
            }
            return Result<void>::Success();
        }

        /** @brief Validates duration, exit clock and enum policies independently of transition identity. */
        bool ValidTransitionPolicy(const GraphStateTransition &transition) {
            return transition.duration.ticks >= 0 && (!transition.exitTime || transition.exitTime->ticks >= 0) &&
                   transition.interruption <= GraphStateInterruption::Either && transition.curve <= GraphStateBlendCurve::EaseOut;
        }

        /** @brief Keeps canonical identity uniqueness separate from the transition's time/predicate policy. */
        bool ValidTransitionIdentity(const GraphStateMachineNode &machine, const std::size_t index) {
            const auto &transition = machine.transitions[index];
            return transition.id.IsValid() && (index == 0 || transition.id != machine.transitions[index - 1].id);
        }

        /** @brief Validates exact parameter references and each condition's typed threshold. */
        Result<void> ValidateConditions(const GraphStateTransition &transition, const AnimationGraphData &data,
                                        GraphSourceLocation location) {
            for (const auto &condition : transition.conditions) {
                const auto *parameter = Find(data.parameters, condition.parameter);
                if (!parameter)
                    return Result<void>::Failure(Failure(AnimationErrors::GraphBindingMismatch, location));
                if (!ValidCondition(condition, *parameter))
                    return Result<void>::Failure(Failure(AnimationErrors::GraphTypeMismatch, location));
            }
            return Result<void>::Success();
        }

        /** @brief Admits the canonical transition table and aggregate condition budget before retaining a node. */
        Result<void> ValidateTransitions(const GraphStateMachineNode &machine, const AnimationGraphData &data,
                                         GraphSourceLocation location) {
            std::uint64_t conditions = 0;
            for (std::size_t index = 0; index < machine.transitions.size(); ++index) {
                const auto &transition = machine.transitions[index];
                if (!ValidTransitionIdentity(machine, index) || !ValidTransitionPolicy(transition))
                    return Result<void>::Failure(Failure(AnimationErrors::GraphMalformed, location));
                if (!Find(machine.states, transition.source) || !Find(machine.states, transition.destination))
                    return Result<void>::Failure(Failure(AnimationErrors::GraphBindingMismatch, location));
                conditions += transition.conditions.size();
                if (conditions > AnimationGraphHardLimits::Conditions)
                    return Result<void>::Failure(Failure(AnimationErrors::GraphLimitExceeded, location));
                if (auto checked = ValidateConditions(transition, data, location); checked.HasError())
                    return checked;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateStateNode */
    Result<void> ValidateStateNode(const GraphStateMachineNode &machine, const AnimationGraphData &data,
                                   const GraphSourceLocation location) {
        const auto fail = [location](const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(Failure(code, location));
        };
        if (machine.states.empty() || machine.states.size() > AnimationGraphHardLimits::States ||
            machine.transitions.size() > AnimationGraphHardLimits::Transitions)
            return fail(AnimationErrors::GraphLimitExceeded);
        if (!Find(machine.states, machine.initial))
            return fail(AnimationErrors::GraphBindingMismatch);
        if (auto checked = ValidateStates(machine, data, location); checked.HasError())
            return checked;
        return ValidateTransitions(machine, data, location);
    }
}  // namespace Horo::Animation::GraphCompileDetail
