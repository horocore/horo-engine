#include "AnimationStateMachineInternal.h"

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Updates a condition's admitted candidate latch using exact typed scalar storage. */
        bool ConditionValue(const StateMachineCondition &condition, const GraphParameterValue &value, const bool latched) noexcept {
            using enum StateMachinePredicate;
            switch (condition.predicate) {
                case Above:
                    return std::get<float>(value) > (latched ? condition.releaseThreshold : std::get<float>(condition.threshold));
                case Below:
                    return std::get<float>(value) < (latched ? condition.releaseThreshold : std::get<float>(condition.threshold));
                case Equal:
                    return value == condition.threshold;
                case NotEqual:
                    return value != condition.threshold;
                case Trigger:
                    return std::get<bool>(value);
            }
            return false;
        }

        /** @brief Admits interruptions only from the current transition's declared source/destination policy. */
        bool EligibleSource(const StateMachinePlan &plan, const StateMachineRuntime &runtime,
                            const StateMachineTransition &transition) noexcept {
            if (!runtime.transition.has_value())
                return transition.source == runtime.source.state;
            const auto &active = plan.transitions[*runtime.transition];
            if (transition.priority <= active.priority)
                return false;
            using enum StateMachineInterruption;
            switch (active.interruption) {
                case None:
                    return false;
                case Source:
                    return transition.source == runtime.source.state;
                case Destination:
                    return transition.source == runtime.destination->state;
                case Either:
                    return transition.source == runtime.source.state || transition.source == runtime.destination->state;
            }
            return false;
        }

        /** @brief Resolves exit time against the sole active player observation for this semantic source. */
        bool ExitTimeReached(const StateMachinePlan &plan, const StateMachineTransition &transition, const StateMachineRuntime &runtime,
                             const StateMachineStepInput &input) noexcept {
            if (!transition.exitTime)
                return true;
            const bool destination = runtime.transition.has_value() &&
                                     (plan.transitions[*runtime.transition].interruption == StateMachineInterruption::Destination ||
                                      transition.source != runtime.source.state);
            const auto slot = destination ? 1U : 0U;
            // Before the first entry, authored elapsed time is exactly zero.
            return slot < input.playback.size() ? input.playback[slot].elapsed.ticks >= transition.exitTime->ticks
                                                : transition.exitTime->ticks == 0;
        }

        /** @brief Every retained condition latch must admit the candidate transition. */
        bool ConditionsSatisfied(const StateMachineTransition &transition, const std::span<const std::uint8_t> latches) noexcept {
            for (std::uint32_t condition = 0; condition < transition.conditions; ++condition)
                if (latches[transition.firstCondition + condition] == 0)
                    return false;
            return true;
        }

        /** @brief Priority wins first; stable transition identity breaks an equal-priority tie. */
        bool PreferredTransition(const StateMachineTransition &candidate, const StateMachineTransition &selected) noexcept {
            if (candidate.priority != selected.priority)
                return candidate.priority > selected.priority;
            return candidate.id < selected.id;
        }
    }  // namespace

    /** @copydoc SelectStateMachineTransition */
    std::optional<std::uint32_t> SelectStateMachineTransition(const StateMachinePlan &plan, const StateMachineRuntime &runtime,
                                                              const StateMachineStepInput &input,
                                                              const std::span<std::uint8_t> latches) noexcept {
        for (std::size_t index = 0; index < plan.conditions.size(); ++index) {
            const auto &condition = plan.conditions[index];
            latches[index] =
                static_cast<std::uint8_t>(ConditionValue(condition, input.parameters[condition.parameter], latches[index] != 0));
        }
        std::optional<std::uint32_t> selected;
        for (std::uint32_t index = 0; index < plan.transitions.size(); ++index) {
            const auto &transition = plan.transitions[index];
            if (!EligibleSource(plan, runtime, transition) || !ExitTimeReached(plan, transition, runtime, input))
                continue;
            if (!ConditionsSatisfied(transition, latches))
                continue;
            if (!selected.has_value() || PreferredTransition(transition, plan.transitions[*selected]))
                selected = index;
        }
        return selected;
    }
}  // namespace Horo::Animation::Detail
