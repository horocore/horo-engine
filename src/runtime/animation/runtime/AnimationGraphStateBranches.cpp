#include "AnimationGraphInstanceInternal.h"

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Checks the prior active subtree while the retiring outer control has already chosen its new state. */
        bool ActiveBeforeControl(const GraphInstanceState &state, const std::uint32_t outer, const GraphInstructionOccurrence &occurrence,
                                 GraphEvaluationInput &input) {
            for (const auto &guard : occurrence.guards) {
                if (guard.machine == outer)
                    continue;
                if (!GraphStateGuardActive(state, guard, true))
                    return false;
                const auto &machine = state.candidate.machines[guard.machine];
                if (machine.held[guard.lane])
                    input.delta = {};
            }
            return true;
        }
    }  // namespace

    /** @copydoc GraphStateBranchOccurrences */
    std::span<const GraphInstructionOccurrence> GraphStateBranchOccurrences(const GraphInstanceState &state, const GraphStateBranch &branch,
                                                                            const std::uint8_t lane) noexcept {
        return state.program->Occurrences().subspan(branch.begin[lane], branch.end[lane] - branch.begin[lane]);
    }

    /** @copydoc TraverseGraphStateBranch */
    GraphInstanceOutcome TraverseGraphStateBranch(GraphInstanceState &state, const std::uint32_t machine, const GraphStateBranch &branch,
                                                  const std::uint8_t lane, const GraphEvaluationInput &input, const bool stageCursors) {
        for (const auto &occurrence : GraphStateBranchOccurrences(state, branch, lane)) {
            if (!occurrence.clipPlayer.has_value())
                continue;
            auto localInput = input;
            if (!ActiveBeforeControl(state, machine, occurrence, localInput))
                continue;
            auto player = state.candidate.players[*occurrence.clipPlayer];
            const auto delta = ScaleGraphDelta(localInput, player);
            if (delta.HasError())
                return {GraphInstanceFailure::ClipFailure, delta.ErrorValue()};
            const auto traversal =
                state.clips[*occurrence.clipBindingIndex]->TraversePrepared(player.cursor, delta.Value(), state.clipLimits);
            if (traversal.HasError())
                return {GraphInstanceFailure::ClipFailure, traversal.ErrorValue()};
            if (stageCursors) {
                player.cursor = traversal.Value().cursor;
                state.candidate.players[*occurrence.clipPlayer] = player;
            }
        }
        return {};
    }

    /** @copydoc RetireGraphStateBranch */
    GraphInstanceOutcome RetireGraphStateBranch(GraphInstanceState &state, const std::uint32_t machine, const GraphStateBranch &branch,
                                                const std::uint8_t lane, const GraphEvaluationInput &input) {
        if (const auto traversed = TraverseGraphStateBranch(state, machine, branch, lane, input, true); traversed.HasError())
            return traversed;
        for (const auto &occurrence : GraphStateBranchOccurrences(state, branch, lane)) {
            if (occurrence.phase != GraphOccurrencePhase::StateBegin)
                continue;
            if (auto localInput = input; !ActiveBeforeControl(state, machine, occurrence, localInput))
                continue;
            auto &nested = state.candidate.machines[*occurrence.stateMachine];
            if (!nested.runtime.initialized || nested.retiring)
                continue;
            const auto &prepared = state.preparedMachines[*occurrence.stateMachine];
            std::array<StateMachineEvent, 3> events;
            std::size_t count = 0;
            StateMachineTransitionId transition{};
            if (nested.runtime.transition.has_value()) {
                transition = prepared.transitions[*nested.runtime.transition].id;
                events[count++] = {StateMachineEventKind::TransitionInterrupted, prepared.states[nested.runtime.source.state].id,
                                   transition, nested.runtime.source.generation};
            }
            events[count++] = {StateMachineEventKind::Exited, prepared.states[nested.runtime.source.state].id, transition,
                               nested.runtime.source.generation};
            if (nested.runtime.destination)
                events[count++] = {StateMachineEventKind::Exited, prepared.states[nested.runtime.destination->state].id, transition,
                                   nested.runtime.destination->generation};
            if (const auto appended = AppendGraphStateEvents(state, occurrence, input, std::span{events}.first(count)); appended.HasError())
                return appended;
            // Keep prior guards readable until the complete subtree has been inspected; clear only at the final execution fence.
            nested.retiring = true;
        }
        return {};
    }
}  // namespace Horo::Animation::Detail
