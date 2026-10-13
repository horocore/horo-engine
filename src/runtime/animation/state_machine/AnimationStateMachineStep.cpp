#include "AnimationStateMachineInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief An unentered machine retains its next incarnation counter but no active transition. */
        bool ValidColdRuntime(const StateMachineRuntime &runtime) noexcept {
            return runtime.source.generation == 0 && !runtime.destination.has_value() && !runtime.transition.has_value() &&
                   runtime.elapsed.ticks == 0 && runtime.nextActivation != 0 && !runtime.frozenSource;
        }

        /** @brief An active transition pins two distinct previously issued incarnations and unfinished time. */
        bool ValidTransitionRuntime(const StateMachinePlan &plan, const StateMachineRuntime &runtime) noexcept {
            if (*runtime.transition >= plan.transitions.size() || !runtime.destination.has_value() ||
                runtime.destination->generation == 0 || runtime.destination->generation >= runtime.nextActivation ||
                runtime.destination->generation == runtime.source.generation)
                return false;
            const auto &active = plan.transitions[*runtime.transition];
            return active.source == runtime.source.state && active.destination == runtime.destination->state &&
                   runtime.elapsed.ticks < active.duration.ticks;
        }

        /** @brief Rejects invalid semantic incarnations before indexing retained immutable tables. */
        bool ValidRuntime(const StateMachinePlan &plan, const StateMachineRuntime &runtime) noexcept {
            if (!runtime.initialized)
                return ValidColdRuntime(runtime);
            if (runtime.source.state >= plan.states.size() || runtime.source.generation == 0 ||
                runtime.source.generation >= runtime.nextActivation || runtime.elapsed.ticks < 0)
                return false;
            if (!runtime.transition.has_value())
                return !runtime.destination.has_value() && runtime.elapsed.ticks == 0 && !runtime.frozenSource;
            return ValidTransitionRuntime(plan, runtime);
        }

        /** @brief Parameter evidence is finite before its typed condition operands may be selected. */
        GraphInstanceFailure ValidateParameters(const StateMachinePlan &plan, const StateMachineStepInput &input) noexcept {
            using enum GraphInstanceFailure;
            for (const auto &value : input.parameters)
                if (const auto *scalar = std::get_if<float>(&value); scalar && !std::isfinite(*scalar))
                    return Malformed;
            for (const auto &condition : plan.conditions) {
                if (condition.parameter >= input.parameters.size())
                    return BindingMismatch;
                if (input.parameters[condition.parameter].index() != condition.threshold.index())
                    return TypeMismatch;
            }
            return None;
        }

        /** @brief Loop evidence is direction-consistent and its exact ordinal interval cannot wrap. */
        GraphInstanceFailure ValidatePlaybackLoops(const StateMachinePlayback &playback) noexcept {
            using enum GraphInstanceFailure;
            if (playback.direction >= AnimationTraversalDirection::Count ||
                (playback.direction == AnimationTraversalDirection::Held && playback.loops != 0))
                return Malformed;
            if (playback.loops == 0)
                return None;
            if (playback.firstLoopOrdinal == 0 ||
                playback.firstLoopOrdinal > std::numeric_limits<std::uint64_t>::max() - (playback.loops - 1ULL))
                return LimitExceeded;
            return None;
        }

        /** @brief Maps one already count-admitted observation to its expected semantic incarnation. */
        StateMachineActivation ExpectedActivation(const StateMachinePlan &plan, const StateMachineRuntime &runtime,
                                                  const std::size_t index) noexcept {
            if (!runtime.initialized)
                return {plan.initial, runtime.nextActivation};
            return index == 0 ? runtime.source : *runtime.destination;
        }

        /** @brief Playback snapshots must identify exactly the currently active semantic incarnations. */
        GraphInstanceFailure ValidatePlayback(const StateMachinePlan &plan, const StateMachineRuntime &runtime,
                                              const StateMachineStepInput &input) noexcept {
            using enum GraphInstanceFailure;
            if (const auto expected =
                    runtime.initialized ? 1U + static_cast<unsigned>(runtime.destination.has_value()) : input.playback.size();
                input.playback.size() != expected || (!runtime.initialized && expected > 1))
                return BindingMismatch;
            std::uint64_t loops = 0;
            for (std::size_t index = 0; index < input.playback.size(); ++index) {
                const auto &playback = input.playback[index];
                if (const auto activation = ExpectedActivation(plan, runtime, index); playback.activation != activation)
                    return HandleStale;
                if (const auto failure = ValidatePlaybackLoops(playback); failure != None)
                    return failure;
                loops += playback.loops;
            }
            return loops > StateMachinePlan::MaximumLoopEvents ? LimitExceeded : None;
        }

        /** @brief Admits fixed-size candidate buffers and bounded parameter transport before testing their contents. */
        bool StepStorageAdmitted(const StateMachinePlan &plan, const StateMachineStepInput &input,
                                 const std::span<const std::uint8_t> latches, const std::span<std::uint8_t> candidateLatches) noexcept {
            return StateMachineStorageAdmitted(plan) && latches.size() == plan.conditions.size() &&
                   candidateLatches.size() == plan.conditions.size() && input.parameters.size() <= AnimationGraphHardLimits::Parameters;
        }

        /** @brief Checks caller-owned typed buffers and exact sole-player evidence without mutating committed storage. */
        GraphInstanceFailure ValidateStep(const StateMachinePlan &plan, const StateMachineRuntime &runtime,
                                          const StateMachineStepInput &input, const std::span<const std::uint8_t> latches,
                                          const std::span<std::uint8_t> candidateLatches) noexcept {
            using enum GraphInstanceFailure;
            if (!StepStorageAdmitted(plan, input, latches, candidateLatches))
                return LimitExceeded;
            if (!latches.empty() && latches.data() == candidateLatches.data())
                return StateInvalid;
            if (!ValidRuntime(plan, runtime) || input.advance.ticks < 0)
                return Malformed;
            for (const auto latch : latches)
                if (latch > 1)
                    return Malformed;
            if (const auto failure = ValidateParameters(plan, input); failure != None)
                return failure;
            return ValidatePlayback(plan, runtime, input);
        }

        /** @brief Projects exact integer blend time through one admitted finite scalar curve. */
        float Weight(const StateMachinePlan &plan, const StateMachineRuntime &runtime) noexcept {
            if (!runtime.transition.has_value())
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
                using enum StateMachineEventKind;
                for (std::uint32_t loop = 0; loop < playback.loops; ++loop)
                    output[count++] = {Loop,
                                       plan.states[playback.activation.state].id,
                                       {},
                                       playback.activation.generation,
                                       playback.firstLoopOrdinal + loop,
                                       playback.direction};
            }
        };

        /** @brief Uses destination-first only when the active policy requires that semantic incarnation. */
        bool UsesDestination(const StateMachinePlan &plan, const StateMachineRuntime &runtime,
                             const StateMachineTransition &selected) noexcept {
            return runtime.transition.has_value() &&
                   (plan.transitions[*runtime.transition].interruption == StateMachineInterruption::Destination ||
                    selected.source != runtime.source.state);
        }

        /** @brief Retires the old transition and begins one bounded new semantic entry. */
        void StartTransition(const StateMachinePlan &plan, StateMachineRuntime &runtime, const std::uint32_t selected,
                             EventWriter &writer) noexcept {
            using enum StateMachineEventKind;
            const auto &next = plan.transitions[selected];
            if (runtime.transition.has_value()) {
                const auto activeId = plan.transitions[*runtime.transition].id;
                writer.State(TransitionInterrupted, runtime.source, activeId);
                const bool fromDestination = UsesDestination(plan, runtime, next);
                writer.State(Exited, fromDestination ? runtime.source : *runtime.destination, activeId);
                if (fromDestination)
                    runtime.source = *runtime.destination;
                runtime.frozenSource = true;
            }
            runtime.destination = StateMachineActivation{next.destination, runtime.nextActivation++};
            runtime.transition = selected;
            runtime.elapsed = {};
            writer.State(TransitionStarted, runtime.source, next.id);
            writer.State(Entered, *runtime.destination, next.id);
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

        /** @brief Computes the complete loop and lifecycle block before writing its first event. */
        std::uint64_t EventCount(const StateMachinePlan &plan, const StateMachineRuntime &runtime, const StateMachineStepInput &input,
                                 const std::optional<std::uint32_t> selected, const bool completing, const bool entering) noexcept {
            std::uint64_t count = entering ? 1U : 0U;
            for (const auto &playback : input.playback)
                count += playback.loops;
            if (selected.has_value())
                count += (runtime.transition.has_value() ? 4U : 2U) + (plan.transitions[*selected].duration.ticks == 0 ? 2U : 0U);
            else if (completing)
                count += 2;
            return count;
        }

        /** @brief Entry counters reserve both initial and destination incarnations without wrapping. */
        bool FitsEntries(const StateMachineRuntime &runtime, const bool selected, const bool entering) noexcept {
            const auto entries = (entering ? 1ULL : 0ULL) + (selected ? 1ULL : 0ULL);
            const auto next = entering ? runtime.nextActivation - 1ULL : runtime.nextActivation;
            return entries <= std::numeric_limits<std::uint64_t>::max() - next;
        }

        /** @brief Reserves the complete event block and its incarnation IDs as one admission decision. */
        bool FitsOutputs(const StateMachinePlan &plan, const StateMachineRuntime &runtime, const StateMachineStepInput &input,
                         const std::optional<std::uint32_t> selected, const bool completing, const std::size_t capacity,
                         const bool entering) noexcept {
            return EventCount(plan, runtime, input, selected, completing, entering) <= capacity &&
                   FitsEntries(runtime, selected.has_value(), entering);
        }

        /** @brief Remaining-duration comparison both detects completion and proves subsequent elapsed addition cannot overflow. */
        bool TransitionCompleting(const StateMachinePlan &plan, const StateMachineRuntime &runtime,
                                  const AnimationTimeDelta advance) noexcept {
            return runtime.transition.has_value() &&
                   advance.ticks >= plan.transitions[*runtime.transition].duration.ticks - runtime.elapsed.ticks;
        }

        /** @brief Establishes a cold machine's first candidate incarnation, retaining all previously issued counters. */
        bool EnterMachine(const StateMachinePlan &plan, StateMachineRuntime &candidate) noexcept {
            if (candidate.initialized)
                return true;
            if (candidate.nextActivation == std::numeric_limits<std::uint64_t>::max())
                return false;
            candidate.source = {plan.initial, candidate.nextActivation++};
            candidate.initialized = true;
            return true;
        }

        /** @brief Mutates only admitted candidate transition state after its entire event block has been reserved. */
        void AdvanceTransition(const StateMachinePlan &plan, StateMachineRuntime &candidate, const StateMachineStepInput &input,
                               const std::optional<std::uint32_t> selected, const bool completing, EventWriter &writer) noexcept {
            if (selected.has_value()) {
                StartTransition(plan, candidate, *selected, writer);
                if (plan.transitions[*selected].duration.ticks == 0)
                    CompleteTransition(plan, candidate, writer);
            } else if (completing) {
                CompleteTransition(plan, candidate, writer);
            } else if (candidate.transition.has_value()) {
                // The remaining-duration comparison proves this sum cannot overflow.
                candidate.elapsed.ticks += input.advance.ticks;
            }
        }
    }  // namespace

    /** @copydoc StepStateMachine */
    StateMachineStepOutcome StepStateMachine(const StateMachinePlan &plan, const StateMachineRuntime &committed,
                                             const std::span<const std::uint8_t> committedLatches, const StateMachineStepInput &input,
                                             StateMachineRuntime &candidate, const std::span<std::uint8_t> candidateLatches,
                                             const std::span<StateMachineEvent> events) noexcept {
        using enum GraphInstanceFailure;
        if (&committed == &candidate)
            return {StateInvalid};
        if (input.cancellation.IsCancellationRequested())
            return {Cancelled};
        if (const auto failure = ValidateStep(plan, committed, input, committedLatches, candidateLatches); failure != None)
            return {failure};
        candidate = committed;
        std::ranges::copy(committedLatches, candidateLatches.begin());
        const bool entering = !candidate.initialized;
        if (!EnterMachine(plan, candidate))
            return {LimitExceeded};
        const auto selected = SelectStateMachineTransition(plan, candidate, input, candidateLatches);
        const bool completing = TransitionCompleting(plan, candidate, input.advance);
        // An initial entry and new destination consume two generations, counted from the original empty state.
        if (!FitsOutputs(plan, candidate, input, selected, completing, events.size(), entering))
            return {LimitExceeded};
        EventWriter writer{plan, events};
        if (entering)
            writer.State(StateMachineEventKind::Entered, candidate.source);
        for (const auto &playback : input.playback)
            writer.Loops(playback);
        const bool interrupting = selected.has_value() && candidate.transition.has_value();
        AdvanceTransition(plan, candidate, input, selected, completing, writer);
        if (input.cancellation.IsCancellationRequested())
            return {Cancelled};
        return {None, writer.count, Weight(plan, candidate), interrupting, selected.has_value()};
    }
}  // namespace Horo::Animation::Detail
