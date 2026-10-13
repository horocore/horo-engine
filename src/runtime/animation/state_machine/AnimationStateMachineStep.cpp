#include "AnimationStateMachineInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Rejects invalid semantic incarnations before indexing retained immutable tables. */
        bool ValidRuntime(const StateMachinePlan &plan, const StateMachineRuntime &runtime) noexcept {
            if (!runtime.initialized)
                return runtime.source.generation == 0 && !runtime.destination && !runtime.transition && runtime.elapsed.ticks == 0 &&
                       runtime.nextActivation != 0 && !runtime.frozenSource;
            if (runtime.source.state >= plan.states.size() || runtime.source.generation == 0 ||
                runtime.source.generation >= runtime.nextActivation || runtime.elapsed.ticks < 0)
                return false;
            if (!runtime.transition)
                return !runtime.destination && runtime.elapsed.ticks == 0 && !runtime.frozenSource;
            if (*runtime.transition >= plan.transitions.size() || !runtime.destination || runtime.destination->generation == 0 ||
                runtime.destination->generation >= runtime.nextActivation || runtime.destination->generation == runtime.source.generation)
                return false;
            const auto &active = plan.transitions[*runtime.transition];
            return active.source == runtime.source.state && active.destination == runtime.destination->state &&
                   runtime.elapsed.ticks < active.duration.ticks;
        }

        /** @brief Checks caller-owned typed buffers and exact sole-player evidence without mutating committed storage. */
        GraphInstanceFailure ValidateStep(const StateMachinePlan &plan, const StateMachineRuntime &runtime,
                                          const StateMachineStepInput &input, const std::span<const std::uint8_t> latches,
                                          const std::span<std::uint8_t> candidateLatches) noexcept {
            using enum GraphInstanceFailure;
            if (plan.states.empty() || plan.states.size() > StateMachinePlan::MaximumStates ||
                plan.transitions.size() > StateMachinePlan::MaximumTransitions ||
                plan.conditions.size() > StateMachinePlan::MaximumConditions || latches.size() != plan.conditions.size() ||
                candidateLatches.size() != plan.conditions.size() || input.parameters.size() > AnimationGraphHardLimits::Parameters)
                return LimitExceeded;
            if (!latches.empty() && latches.data() == candidateLatches.data())
                return StateInvalid;
            if (!ValidRuntime(plan, runtime) || input.advance.ticks < 0)
                return Malformed;
            for (const auto latch : latches)
                if (latch > 1)
                    return Malformed;
            for (const auto &value : input.parameters)
                if (const auto *scalar = std::get_if<float>(&value); scalar && !std::isfinite(*scalar))
                    return Malformed;
            for (const auto &condition : plan.conditions) {
                if (condition.parameter >= input.parameters.size())
                    return BindingMismatch;
                if (input.parameters[condition.parameter].index() != condition.threshold.index())
                    return TypeMismatch;
            }
            const auto expected = runtime.initialized ? (runtime.destination ? 2U : 1U) : input.playback.size();
            if (input.playback.size() != expected || (!runtime.initialized && expected > 1))
                return BindingMismatch;
            std::uint64_t loops = 0;
            for (std::size_t index = 0; index < input.playback.size(); ++index) {
                const auto &playback = input.playback[index];
                const auto activation = !runtime.initialized ? StateMachineActivation{plan.initial, runtime.nextActivation}
                                        : index == 0         ? runtime.source
                                                             : *runtime.destination;
                if (playback.activation != activation)
                    return HandleStale;
                if (playback.direction >= AnimationTraversalDirection::Count ||
                    (playback.direction == AnimationTraversalDirection::Held && playback.loops != 0))
                    return Malformed;
                if (playback.loops != 0 &&
                    (playback.firstLoopOrdinal == 0 ||
                     playback.firstLoopOrdinal > std::numeric_limits<std::uint64_t>::max() - (playback.loops - 1ULL)))
                    return LimitExceeded;
                loops += playback.loops;
            }
            return loops > StateMachinePlan::MaximumLoopEvents ? LimitExceeded : None;
        }

        /** @brief Projects exact integer blend time through one admitted finite scalar curve. */
        float Weight(const StateMachinePlan &plan, const StateMachineRuntime &runtime) noexcept {
            if (!runtime.transition)
                return 0.0F;
            const auto &transition = plan.transitions[*runtime.transition];
            const auto fraction = static_cast<double>(runtime.elapsed.ticks) / static_cast<double>(transition.duration.ticks);
            double weight = fraction;
            using enum StateMachineBlendCurve;
            switch (transition.curve) {
                case Linear:
                    break;
                case SmoothStep:
                    weight = fraction * fraction * (3.0 - 2.0 * fraction);
                    break;
                case EaseIn:
                    weight = fraction * fraction;
                    break;
                case EaseOut:
                    weight = fraction * (2.0 - fraction);
                    break;
            }
            return static_cast<float>(std::clamp(weight, 0.0, 1.0));
        }

        /** @brief Owns only invocation-scoped admitted candidate event bytes, never callbacks or publication. */
        struct EventWriter final {
            const StateMachinePlan &plan;
            std::span<StateMachineEvent> output;
            std::uint32_t count{};

            void State(const StateMachineEventKind kind, const StateMachineActivation activation,
                       const StateMachineTransitionId transition = {}) noexcept {
                output[count++] = {kind, plan.states[activation.state].id, transition, activation.generation};
            }

            void Loops(const StateMachinePlayback &playback) noexcept {
                for (std::uint32_t loop = 0; loop < playback.loops; ++loop)
                    output[count++] = {StateMachineEventKind::Loop,    plan.states[playback.activation.state].id, {},
                                       playback.activation.generation, playback.firstLoopOrdinal + loop,          playback.direction};
            }
        };

        /** @brief Uses destination-first only when the active policy requires that semantic incarnation. */
        bool UsesDestination(const StateMachinePlan &plan, const StateMachineRuntime &runtime,
                             const StateMachineTransition &selected) noexcept {
            return runtime.transition && (plan.transitions[*runtime.transition].interruption == StateMachineInterruption::Destination ||
                                          selected.source != runtime.source.state);
        }

        /** @brief Retires the old transition and begins one bounded new semantic entry. */
        void StartTransition(const StateMachinePlan &plan, StateMachineRuntime &runtime, const std::uint32_t selected,
                             EventWriter &writer) noexcept {
            const auto &next = plan.transitions[selected];
            if (runtime.transition) {
                const auto activeId = plan.transitions[*runtime.transition].id;
                writer.State(StateMachineEventKind::TransitionInterrupted, runtime.source, activeId);
                const bool fromDestination = UsesDestination(plan, runtime, next);
                writer.State(StateMachineEventKind::Exited, fromDestination ? runtime.source : *runtime.destination, activeId);
                if (fromDestination)
                    runtime.source = *runtime.destination;
                runtime.frozenSource = true;
            }
            runtime.destination = StateMachineActivation{next.destination, runtime.nextActivation++};
            runtime.transition = selected;
            runtime.elapsed = {};
            writer.State(StateMachineEventKind::TransitionStarted, runtime.source, next.id);
            writer.State(StateMachineEventKind::Entered, *runtime.destination, next.id);
        }

        /** @brief Completes a transition once; the destination's active player survives without reset. */
        void CompleteTransition(const StateMachinePlan &plan, StateMachineRuntime &runtime, EventWriter &writer) noexcept {
            const auto transition = plan.transitions[*runtime.transition].id;
            writer.State(StateMachineEventKind::Exited, runtime.source, transition);
            writer.State(StateMachineEventKind::TransitionCompleted, *runtime.destination, transition);
            runtime.source = *runtime.destination;
            runtime.destination.reset();
            runtime.transition.reset();
            runtime.elapsed = {};
            runtime.frozenSource = false;
        }

        /** @brief Preflights exact event/entry counts before any event buffer writes or activation generation increments. */
        bool FitsOutputs(const StateMachinePlan &plan, const StateMachineRuntime &runtime, const StateMachineStepInput &input,
                         const std::optional<std::uint32_t> selected, const bool completing, const std::size_t capacity,
                         const bool entering) noexcept {
            std::uint64_t count = entering ? 1U : 0U;
            for (const auto &playback : input.playback)
                count += playback.loops;
            if (selected)
                count += (runtime.transition ? 4U : 2U) + (plan.transitions[*selected].duration.ticks == 0 ? 2U : 0U);
            else if (completing)
                count += 2;
            const auto entries = (entering ? 1ULL : 0ULL) + (selected ? 1ULL : 0ULL);
            const auto next = entering ? runtime.nextActivation - 1ULL : runtime.nextActivation;
            return count <= capacity && entries <= std::numeric_limits<std::uint64_t>::max() - next;
        }
    }  // namespace

    /** @copydoc StepStateMachine */
    StateMachineStepOutcome StepStateMachine(const StateMachinePlan &plan, const StateMachineRuntime &committed,
                                             const std::span<const std::uint8_t> committedLatches, const StateMachineStepInput &input,
                                             StateMachineRuntime &candidate, const std::span<std::uint8_t> candidateLatches,
                                             const std::span<StateMachineEvent> events) noexcept {
        if (&committed == &candidate)
            return {GraphInstanceFailure::StateInvalid};
        if (input.cancellation.IsCancellationRequested())
            return {GraphInstanceFailure::Cancelled};
        if (const auto failure = ValidateStep(plan, committed, input, committedLatches, candidateLatches);
            failure != GraphInstanceFailure::None)
            return {failure};
        candidate = committed;
        std::ranges::copy(committedLatches, candidateLatches.begin());
        const bool entering = !candidate.initialized;
        if (entering) {
            if (candidate.nextActivation == std::numeric_limits<std::uint64_t>::max())
                return {GraphInstanceFailure::LimitExceeded};
            candidate.source = {plan.initial, candidate.nextActivation++};
            candidate.initialized = true;
        }
        const auto selected = SelectStateMachineTransition(plan, candidate, input, candidateLatches);
        const bool completing =
            candidate.transition && input.advance.ticks >= plan.transitions[*candidate.transition].duration.ticks - candidate.elapsed.ticks;
        // An initial entry and new destination consume two generations, counted from the original empty state.
        if (!FitsOutputs(plan, candidate, input, selected, completing, events.size(), entering))
            return {GraphInstanceFailure::LimitExceeded};
        EventWriter writer{plan, events};
        if (entering)
            writer.State(StateMachineEventKind::Entered, candidate.source);
        for (const auto &playback : input.playback)
            writer.Loops(playback);
        const bool interrupting = selected && candidate.transition.has_value();
        if (selected) {
            StartTransition(plan, candidate, *selected, writer);
            if (plan.transitions[*selected].duration.ticks == 0)
                CompleteTransition(plan, candidate, writer);
        } else if (completing) {
            CompleteTransition(plan, candidate, writer);
        } else if (candidate.transition) {
            // The remaining-duration comparison proves this sum cannot overflow.
            candidate.elapsed.ticks += input.advance.ticks;
        }
        if (input.cancellation.IsCancellationRequested())
            return {GraphInstanceFailure::Cancelled};
        return {GraphInstanceFailure::None, writer.count, Weight(plan, candidate), interrupting, selected.has_value()};
    }
}  // namespace Horo::Animation::Detail
