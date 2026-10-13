#include "AnimationGraphCompilerInternal.h"

#include <cmath>

namespace Horo::Animation::GraphCompileDetail {
    namespace {
        /** @brief Validates a typed predicate before the runtime may use its dense parameter access. */
        bool ValidCondition(const GraphStateCondition &condition, const GraphParameter &parameter) {
            if (condition.predicate > GraphStatePredicate::Trigger || !std::isfinite(condition.releaseThreshold))
                return false;
            using enum GraphStatePredicate;
            if (condition.predicate == Above || condition.predicate == Below) {
                const auto *threshold = std::get_if<float>(&condition.threshold);
                return parameter.type == GraphValueType::Float && threshold && std::isfinite(*threshold) &&
                       (condition.predicate == Above ? condition.releaseThreshold <= *threshold : condition.releaseThreshold >= *threshold);
            }
            if (condition.predicate == Trigger)
                return parameter.type == GraphValueType::Trigger && std::get_if<bool>(&condition.threshold) &&
                       std::get<bool>(condition.threshold);
            return (parameter.type == GraphValueType::Boolean || parameter.type == GraphValueType::Integer) &&
                   condition.threshold.index() == parameter.defaultValue.index();
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
        for (std::size_t index = 0; index < machine.states.size(); ++index) {
            const auto &state = machine.states[index];
            if (!state.id.IsValid() || (index != 0 && state.id == machine.states[index - 1].id))
                return fail(AnimationErrors::GraphMalformed);
            if (const auto *clip = std::get_if<AnimationClipId>(&state.source)) {
                if (!clip->IsValid() || clip->Asset() == data.id.Asset() || clip->Asset() == data.skeleton.Asset() || state.loopPeriod)
                    return fail(AnimationErrors::GraphBindingMismatch);
            } else {
                const auto *definition = Find(data.definitions, std::get<GraphDefinitionId>(state.source));
                if (!definition || !definition->inputs.empty())
                    return fail(AnimationErrors::GraphBindingMismatch);
                if (state.loopPeriod && state.loopPeriod->ticks <= 0)
                    return fail(AnimationErrors::GraphMalformed);
            }
        }
        std::uint64_t conditions = 0;
        for (std::size_t index = 0; index < machine.transitions.size(); ++index) {
            const auto &transition = machine.transitions[index];
            if (!transition.id.IsValid() || (index != 0 && transition.id == machine.transitions[index - 1].id) ||
                transition.duration.ticks < 0 || (transition.exitTime && transition.exitTime->ticks < 0) ||
                transition.interruption > GraphStateInterruption::Either || transition.curve > GraphStateBlendCurve::EaseOut)
                return fail(AnimationErrors::GraphMalformed);
            if (!Find(machine.states, transition.source) || !Find(machine.states, transition.destination))
                return fail(AnimationErrors::GraphBindingMismatch);
            conditions += transition.conditions.size();
            if (conditions > AnimationGraphHardLimits::Conditions)
                return fail(AnimationErrors::GraphLimitExceeded);
            for (const auto &condition : transition.conditions) {
                const auto *parameter = Find(data.parameters, condition.parameter);
                if (!parameter)
                    return fail(AnimationErrors::GraphBindingMismatch);
                if (!ValidCondition(condition, *parameter))
                    return fail(AnimationErrors::GraphTypeMismatch);
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Animation::GraphCompileDetail
