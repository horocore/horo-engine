#include "AnimationGraphInstanceInternal.h"
#include "AnimationPoseComposition.h"

#include <algorithm>
#include <limits>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Resolves the actual admitted authoring opcode for one immutable machine occurrence. */
        const GraphStateMachineNode &Definition(const GraphInstanceState &state, const std::uint32_t machine) {
            const auto &occurrence = state.program->Occurrences()[state.program->StateMachines()[machine].beginOccurrence];
            return std::get<GraphStateMachineNode>(
                state.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex].payload);
        }

        /** @brief Resolves a stable state identity in canonical compiler-owned storage. */
        std::uint32_t StateIndex(const GraphStateMachineNode &machine, const GraphStateId id) {
            return static_cast<std::uint32_t>(std::ranges::lower_bound(machine.states, id, {}, &GraphState::id) - machine.states.begin());
        }

        /** @brief Integer floor division keeps signed state-clock cycle observation exact through reverse playback. */
        std::int64_t Cycle(const std::int64_t time, const std::int64_t period) noexcept {
            const auto quotient = time / period;
            return time % period < 0 ? quotient - 1 : quotient;
        }

        /** @brief Proves exact signed clock addition before mutating the candidate clock. */
        bool ClockDeltaFits(const std::int64_t previous, const std::int64_t delta) noexcept {
            if (delta > 0)
                return previous <= std::numeric_limits<std::int64_t>::max() - delta;
            if (delta < 0)
                return previous >= std::numeric_limits<std::int64_t>::min() - delta;
            return true;
        }

        /** @brief Captures exact loop crossings after admitted signed clock advancement, with bounded nonwrapping ordinals. */
        GraphInstanceOutcome ObserveLoops(GraphStateClock &clock, const std::int64_t previous, const AnimationDeltaTime period,
                                          StateMachinePlayback &observation) {
            const auto before = Cycle(previous, period.ticks);
            const auto after = Cycle(clock.elapsed.ticks, period.ticks);
            const auto loops = after >= before ? static_cast<std::uint64_t>(after) - static_cast<std::uint64_t>(before)
                                               : static_cast<std::uint64_t>(before) - static_cast<std::uint64_t>(after);
            if (loops > StateMachinePlan::MaximumLoopEvents || loops > std::numeric_limits<std::uint64_t>::max() - clock.loopOrdinal)
                return {GraphInstanceFailure::LimitExceeded};
            observation.loops = static_cast<std::uint32_t>(loops);
            if (loops != 0)
                observation.firstLoopOrdinal = clock.loopOrdinal + 1;
            clock.loopOrdinal += loops;
            return {};
        }

        /** @brief Projects exact signed clock movement to its typed traversal direction. */
        AnimationTraversalDirection TraversalDirection(const AnimationTimeDelta delta) noexcept {
            using enum AnimationTraversalDirection;
            if (delta.ticks == 0)
                return Held;
            return delta.ticks > 0 ? Forward : Reverse;
        }

        /** @brief Advances only the admitted state clock; actual pose players retain their sole independent cursors. */
        GraphInstanceOutcome AdvanceClock(GraphStateClock &clock, const GraphEvaluationInput &input,
                                          const std::optional<AnimationDeltaTime> period, const StateMachineActivation activation,
                                          StateMachinePlayback &observation, AnimationTimeDelta &delta) {
            const auto scaled = ScaleGraphDelta(input, clock.rate);
            if (scaled.HasError())
                return {GraphInstanceFailure::ClipFailure, scaled.ErrorValue()};
            delta = scaled.Value();
            const auto previous = clock.elapsed.ticks;
            if (!ClockDeltaFits(previous, delta.ticks))
                return {GraphInstanceFailure::LimitExceeded};
            clock.elapsed.ticks += delta.ticks;
            observation = {activation, clock.elapsed, 0, 0, TraversalDirection(delta)};
            if (period)
                return ObserveLoops(clock, previous, *period, observation);
            return {};
        }

        /** @brief Resets exactly the compiler's newly entered branch, including nested candidate machines and clip players. */
        void ResetBranch(GraphInstanceState &state, const GraphStateBranch &branch, const std::uint8_t lane) {
            for (const auto &occurrence : GraphStateBranchOccurrences(state, branch, lane)) {
                if (occurrence.clipPlayer.has_value())
                    state.candidate.players[*occurrence.clipPlayer] = {};
                if (occurrence.phase == GraphOccurrencePhase::StateBegin) {
                    ResetGraphStateControl(state, *occurrence.stateMachine);
                }
            }
        }

        /** @brief Complete stack-owned clock observations consumed by exactly one control step. */
        struct ControlObservation final {
            std::array<StateMachinePlayback, 2> playback;
            std::size_t observations{};
            AnimationTimeDelta advance{};
        };

        /** @brief Detached old semantic authority used after candidate mutation; never aliases the new runtime/lanes. */
        struct ControlBaseline final {
            StateMachineRuntime runtime;
            std::uint8_t sourceLane{};
            std::uint8_t destinationLane{};
        };

        /** @brief Invocation-scoped references into pinned program tables and fixed-size candidate storage; never retained. */
        struct ControlContext final {
            GraphInstanceState &state;
            const GraphInstructionOccurrence &occurrence;
            std::uint32_t index;
            const GraphStateMachineBinding &binding;
            const GraphPreparedMachine &prepared;
            GraphMachineState &candidate;
        };

        /** @brief Resolves one immutable control binding once for all phases of its tick transaction. */
        ControlContext Control(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence) {
            const auto index = *occurrence.stateMachine;
            return {state,
                    occurrence,
                    index,
                    state.program->StateMachines()[index],
                    state.preparedMachines[index],
                    state.candidate.machines[index]};
        }

        /** @brief Proves the old active clip ranges remain admissible even if this tick cuts them away. */
        GraphInstanceOutcome PreflightControl(const ControlContext &control, const GraphEvaluationInput &input,
                                              const ControlBaseline &baseline) {
            const auto sourceState = baseline.runtime.initialized ? baseline.runtime.source.state : control.prepared.initial;
            const auto sourceLane = baseline.runtime.initialized ? baseline.sourceLane : 0U;
            if (const auto preflight = TraverseGraphStateBranch(control.state, control.index, control.binding.branches[sourceState],
                                                                static_cast<std::uint8_t>(sourceLane), input, false);
                preflight.HasError())
                return preflight;
            if (baseline.runtime.destination)
                if (const auto preflight = TraverseGraphStateBranch(control.state, control.index,
                                                                    control.binding.branches[baseline.runtime.destination->state],
                                                                    baseline.destinationLane, input, false);
                    preflight.HasError())
                    return preflight;
            return {};
        }

        /** @brief Captures exact source/destination authored clocks before transition selection. */
        GraphInstanceOutcome ObserveControl(const ControlContext &control, const GraphEvaluationInput &input,
                                            const ControlBaseline &baseline, ControlObservation &observation) {
            auto &playback = observation.playback;
            auto &observations = observation.observations;
            auto &advance = observation.advance;
            if (baseline.runtime.initialized) {
                if (const auto observed = AdvanceClock(control.candidate.clocks[baseline.sourceLane], input,
                                                       control.binding.branches[baseline.runtime.source.state].loopPeriod,
                                                       baseline.runtime.source, playback[0], advance);
                    observed.HasError())
                    return observed;
                observations = 1;
                if (baseline.runtime.destination) {
                    AnimationTimeDelta targetAdvance;
                    if (const auto observed = AdvanceClock(control.candidate.clocks[baseline.destinationLane], input,
                                                           control.binding.branches[baseline.runtime.destination->state].loopPeriod,
                                                           *baseline.runtime.destination, playback[1], targetAdvance);
                        observed.HasError())
                        return observed;
                    observations = 2;
                }
            } else {
                if (const auto observed =
                        AdvanceClock(control.candidate.clocks[0], input, control.binding.branches[control.prepared.initial].loopPeriod,
                                     {control.prepared.initial, baseline.runtime.nextActivation}, playback[0], advance);
                    observed.HasError())
                    return observed;
                observations = 1;
            }
            if (advance.ticks == std::numeric_limits<std::int64_t>::min())
                return {GraphInstanceFailure::LimitExceeded};
            return {};
        }

        /** @brief Stages latches/control and copies the complete bounded lifecycle block into the tick candidate. */
        GraphInstanceOutcome StepControl(const ControlContext &control, const GraphEvaluationInput &input, const ControlBaseline &baseline,
                                         const ControlObservation &observation) {
            const auto &playback = observation.playback;
            const auto observations = observation.observations;
            const auto advance = observation.advance;
            const auto count = control.prepared.conditions.size();
            const auto latches = std::span{control.state.candidate.latches}.subspan(control.binding.conditionOffset, count);
            const auto scratch = std::span{control.state.latchScratch}.subspan(control.binding.conditionOffset, count);
            std::ranges::copy(latches, scratch.begin());
            std::array<StateMachineEvent, 262> events;
            control.candidate.outcome = StepStateMachine(control.prepared.Plan(), baseline.runtime, scratch,
                                                         {control.state.candidate.parameters,
                                                          std::span{playback}.first(observations),
                                                          {advance.ticks < 0 ? -advance.ticks : advance.ticks},
                                                          input.cancellation},
                                                         control.candidate.runtime, latches, events);
            if (control.candidate.outcome.failure != GraphInstanceFailure::None)
                return {control.candidate.outcome.failure};
            control.candidate.visited = true;
            if (const auto appended = AppendGraphStateEvents(control.state, control.occurrence, input,
                                                             std::span{events}.first(control.candidate.outcome.events));
                appended.HasError())
                return appended;
            return {};
        }

        /** @brief Identity, not state index or pose lane, determines whether a semantic incarnation survives. */
        bool RetainsActivation(const StateMachineRuntime &runtime, const std::uint64_t generation) noexcept {
            return runtime.source.generation == generation || (runtime.destination && runtime.destination->generation == generation);
        }

        /** @brief Retires only semantic incarnations absent from the newly staged source/destination pair. */
        GraphInstanceOutcome RetireControl(const ControlContext &control, const GraphEvaluationInput &input,
                                           const ControlBaseline &baseline) {
            if (const auto oldSource = baseline.runtime.initialized ? baseline.runtime.source.generation : baseline.runtime.nextActivation;
                !RetainsActivation(control.candidate.runtime, oldSource))
                if (const auto retired =
                        RetireGraphStateBranch(control.state, control.index,
                                               control.binding.branches[baseline.runtime.initialized ? baseline.runtime.source.state
                                                                                                     : control.prepared.initial],
                                               baseline.runtime.initialized ? baseline.sourceLane : 0, input);
                    retired.HasError())
                    return retired;
            if (baseline.runtime.destination && !RetainsActivation(control.candidate.runtime, baseline.runtime.destination->generation))
                if (const auto retired =
                        RetireGraphStateBranch(control.state, control.index, control.binding.branches[baseline.runtime.destination->state],
                                               baseline.destinationLane, input);
                    retired.HasError())
                    return retired;
            return {};
        }

        /** @brief Applies entry resets and committed-source capture after old subtree retirement succeeds. */
        void ActivateControl(const ControlContext &control, const ControlBaseline &baseline) {
            std::ranges::copy(control.state.Pose(control.binding.poses[2]), control.state.Pose(control.binding.poses[3]).begin());
            std::ranges::copy(control.state.Coverage(control.binding.poses[2]), control.state.Coverage(control.binding.poses[3]).begin());
            if (!baseline.runtime.initialized) {
                control.candidate.sourceLane = 0;
                control.candidate.destinationLane = 1;
                ResetBranch(control.state, control.binding.branches[control.prepared.initial], 0);
            }
            if (control.candidate.outcome.resetDestination) {
                if (baseline.runtime.destination && control.candidate.runtime.source.generation == baseline.runtime.destination->generation)
                    control.candidate.sourceLane = baseline.destinationLane;
                control.candidate.destinationLane = static_cast<std::uint8_t>(1U - control.candidate.sourceLane);
                const auto entered = control.candidate.runtime.destination.value_or(control.candidate.runtime.source);
                const auto lane = control.candidate.destinationLane;
                control.candidate.clocks[lane] = {};
                control.candidate.held[lane] = true;
                ResetBranch(control.state, control.binding.branches[entered.state], lane);
                if (control.candidate.outcome.captureSource) {
                    std::ranges::copy(control.state.Pose(control.binding.poses[0]), control.state.Pose(control.binding.poses[3]).begin());
                    std::ranges::copy(control.state.Coverage(control.binding.poses[0]),
                                      control.state.Coverage(control.binding.poses[3]).begin());
                }
                if (!control.candidate.runtime.transition.has_value())
                    control.candidate.sourceLane = lane;
            } else if (baseline.runtime.destination && !control.candidate.runtime.transition.has_value()) {
                control.candidate.sourceLane = baseline.destinationLane;
                control.candidate.destinationLane = static_cast<std::uint8_t>(1U - control.candidate.sourceLane);
            }
        }

    }  // namespace

    /** @copydoc AppendGraphStateEvents */
    GraphInstanceOutcome AppendGraphStateEvents(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                                const GraphEvaluationInput &input, const std::span<const StateMachineEvent> events) {
        if (events.size() > state.candidate.events.size() - state.candidate.eventCount)
            return {GraphInstanceFailure::LimitExceeded};
        for (const auto &event : events) {
            const auto ordinal = state.candidate.eventCount++;
            state.candidate.events[ordinal] = {state.identity,
                                               input.tick,
                                               occurrence.source,
                                               state.program->StateMachines()[*occurrence.stateMachine].beginOccurrence,
                                               ordinal,
                                               event.kind,
                                               GraphStateId::Create(event.state.value).Value(),
                                               event.transition.value == 0 ? GraphTransitionId{}
                                                                           : GraphTransitionId::Create(event.transition.value).Value(),
                                               event.activation,
                                               event.loopOrdinal,
                                               event.direction};
        }
        return {};
    }

    /** @copydoc PrepareGraphStateMachines */
    GraphInstanceFailure PrepareGraphStateMachines(GraphInstanceState &state) {
        const auto &memory = state.program->Memory();
        state.machines.resize(memory.stateMachines);
        state.candidate.machines.resize(memory.stateMachines);
        state.latches.resize(memory.conditionLatches);
        state.latchScratch.resize(memory.conditionLatches);
        state.candidate.latches.resize(memory.conditionLatches);
        state.candidate.events.resize(memory.stateEvents);
        for (auto &events : state.published.events)
            events.resize(memory.stateEvents);
        state.preparedMachines.reserve(memory.stateMachines);
        for (std::uint32_t index = 0; index < memory.stateMachines; ++index) {
            const auto &machine = Definition(state, index);
            GraphPreparedMachine prepared;
            prepared.initial = StateIndex(machine, machine.initial);
            prepared.states.reserve(machine.states.size());
            prepared.transitions.reserve(machine.transitions.size());
            std::size_t conditionCount = 0;
            for (const auto &transition : machine.transitions)
                conditionCount += transition.conditions.size();
            prepared.conditions.reserve(conditionCount);
            for (const auto &entry : machine.states)
                prepared.states.push_back({{entry.id.Value()}});
            for (const auto &transition : machine.transitions) {
                StateMachineTransition compiled{{transition.id.Value()},
                                                StateIndex(machine, transition.source),
                                                StateIndex(machine, transition.destination),
                                                transition.priority,
                                                static_cast<std::uint32_t>(prepared.conditions.size()),
                                                static_cast<std::uint32_t>(transition.conditions.size()),
                                                transition.duration,
                                                transition.exitTime,
                                                transition.interruption,
                                                transition.curve};
                for (const auto &condition : transition.conditions) {
                    const auto declarations = state.program->Parameters();
                    const auto parameter = std::ranges::lower_bound(declarations, condition.parameter, {}, &GraphParameter::id);
                    prepared.conditions.push_back({static_cast<std::uint32_t>(parameter - declarations.begin()), condition.predicate,
                                                   condition.threshold, condition.releaseThreshold});
                }
                prepared.transitions.push_back(compiled);
            }
            if (const auto failure = ValidateStateMachine(prepared.Plan(), state.program->Parameters());
                failure != GraphInstanceFailure::None)
                return failure;
            state.preparedMachines.push_back(std::move(prepared));
        }
        return GraphInstanceFailure::None;
    }

    /** @copydoc ResetGraphStateControl */
    void ResetGraphStateControl(GraphInstanceState &state, const std::uint32_t machine) noexcept {
        const auto nextActivation = state.candidate.machines[machine].runtime.nextActivation;
        state.candidate.machines[machine] = {};
        state.candidate.machines[machine].runtime.nextActivation = nextActivation;
        const auto &binding = state.program->StateMachines()[machine];
        std::ranges::fill(std::span{state.candidate.latches}.subspan(binding.conditionOffset,
                                                                     state.preparedMachines[machine].conditions.size()),
                          0);
    }

    /** @copydoc GraphStateGuardActive */
    bool GraphStateGuardActive(const GraphInstanceState &state, const GraphStateGuard &guard, const bool allowCold) noexcept {
        const auto &machine = state.candidate.machines[guard.machine];
        if (!machine.runtime.initialized)
            return allowCold && guard.lane == 0 && guard.state == state.preparedMachines[guard.machine].initial;
        const bool source = guard.lane == machine.sourceLane && guard.state == machine.runtime.source.state;
        const bool destination =
            machine.runtime.destination && guard.lane == machine.destinationLane && guard.state == machine.runtime.destination->state;
        return source || destination;
    }

    /** @copydoc StateBranchInput */
    bool StateBranchInput(const GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                          GraphEvaluationInput &input) noexcept {
        for (const auto &guard : occurrence.guards) {
            const auto &machine = state.candidate.machines[guard.machine];
            if (!GraphStateGuardActive(state, guard, false))
                return false;
            if (machine.held[guard.lane])
                input.delta = {};
        }
        return true;
    }

    /** @copydoc BeginGraphStateMachine */
    GraphInstanceOutcome BeginGraphStateMachine(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                                const GraphEvaluationInput &input) {
        const auto control = Control(state, occurrence);
        const ControlBaseline baseline{control.candidate.runtime, control.candidate.sourceLane, control.candidate.destinationLane};
        if (const auto outcome = PreflightControl(control, input, baseline); outcome.HasError())
            return outcome;
        ControlObservation observation;
        if (const auto outcome = ObserveControl(control, input, baseline, observation); outcome.HasError())
            return outcome;
        if (const auto outcome = StepControl(control, input, baseline, observation); outcome.HasError())
            return outcome;
        if (const auto outcome = RetireControl(control, input, baseline); outcome.HasError())
            return outcome;
        ActivateControl(control, baseline);
        return {};
    }

    /** @copydoc EndGraphStateMachine */
    GraphInstanceOutcome EndGraphStateMachine(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence) {
        const auto index = *occurrence.stateMachine;
        const auto &binding = state.program->StateMachines()[index];
        const auto &candidate = state.candidate.machines[index];
        const auto output = state.Pose(occurrence.output);
        const auto first = candidate.runtime.frozenSource
                               ? state.Pose(binding.poses[3])
                               : state.Pose(binding.branches[candidate.runtime.source.state].output[candidate.sourceLane]);
        const auto firstCoverage = candidate.runtime.frozenSource
                                       ? state.Coverage(binding.poses[3])
                                       : state.Coverage(binding.branches[candidate.runtime.source.state].output[candidate.sourceLane]);
        const auto coverage = state.Coverage(occurrence.output);
        if (candidate.runtime.destination) {
            const auto second = state.Pose(binding.branches[candidate.runtime.destination->state].output[candidate.destinationLane]);
            if (const auto failure = ComposePose({.base = first, .overlay = second, .weight = candidate.outcome.weight}, output);
                failure != GraphInstanceFailure::None)
                return {failure};
            const auto secondCoverage =
                state.Coverage(binding.branches[candidate.runtime.destination->state].output[candidate.destinationLane]);
            for (std::size_t joint = 0; joint < coverage.size(); ++joint)
                coverage[joint] = static_cast<std::uint8_t>((candidate.outcome.weight < 1.0F && firstCoverage[joint] != 0) ||
                                                            (candidate.outcome.weight > 0.0F && secondCoverage[joint] != 0));
        } else {
            std::ranges::copy(first, output.begin());
            std::ranges::copy(firstCoverage, coverage.begin());
        }
        std::ranges::copy(output, state.Pose(binding.poses[1]).begin());
        std::ranges::copy(coverage, state.Coverage(binding.poses[1]).begin());
        return {};
    }
}  // namespace Horo::Animation::Detail
