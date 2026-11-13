#include "AnimationGraphInstanceInternal.h"

#include <array>

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

    namespace {
        /** @brief Preflights/stages one original player, shared by ordinary clips and tree members during branch retirement. */
        GraphInstanceOutcome TraversePlayer(GraphInstanceState &state, const std::uint32_t playerIndex, const std::uint32_t clipIndex,
                                            const GraphEvaluationInput &input, const bool stage,
                                            const GraphInstructionOccurrence &occurrence) {
            auto player = state.candidate.players[playerIndex];
            const auto delta = ScaleGraphDelta(input, player);
            if (delta.HasError())
                return {GraphInstanceFailure::ClipFailure, delta.ErrorValue()};
            const auto traversal = state.clips[clipIndex]->TraversePrepared(player.cursor, delta.Value(), state.clipLimits);
            if (traversal.HasError())
                return {GraphInstanceFailure::ClipFailure, traversal.ErrorValue()};
            if (stage) {
                const auto &descriptor = state.clips[clipIndex]->Data().descriptor;
                if (const auto notified =
                        StageClipNotifies(state, playerIndex, clipIndex, player.cursor, delta.Value(),
                                          {{0}, {descriptor.duration.ticks}, descriptor.wrapMode}, ClipNotifyEligible(state, occurrence));
                    notified.HasError())
                    return notified;
                player.cursor = traversal.Value().cursor;
                state.candidate.players[playerIndex] = player;
            }
            return {};
        }

        /** @brief Retiring tree branches preflight only positive current-coordinate contributions through the shared weight authority. */
        GraphInstanceOutcome TraverseTree(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                          const GraphEvaluationInput &input, const bool stage) {
            const auto &tree = std::get<GraphBlendTreeNode>(
                state.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex].payload);
            const auto x = std::get<float>(state.candidate.parameters[occurrence.blendParameters[0]]);
            const auto y = tree.dimension == GraphBlendDimension::Two
                               ? std::get<float>(state.candidate.parameters[occurrence.blendParameters[1]])
                               : 0.0F;
            std::array<float, AnimationGraphHardLimits::BlendSamplesPerTree> weights{};
            if (const auto result = ComputeGraphBlendWeights(tree, x, y, std::span{weights}.first(tree.samples.size())); result.HasError())
                return result;
            for (std::size_t i = 0; i < occurrence.blendSamples.size(); ++i) {
                if (weights[i] <= 0.0F)
                    continue;
                const auto &sample = occurrence.blendSamples[i];
                if (const auto result = TraversePlayer(state, sample.player, sample.clip, input, stage, occurrence); result.HasError())
                    return result;
            }
            return {};
        }

        /** @brief Includes all compiler-admitted player ownership in one active occurrence. */
        GraphInstanceOutcome TraverseOccurrence(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                                const GraphEvaluationInput &input, const bool stage) {
            if (occurrence.clipPlayer.has_value())
                return TraversePlayer(state, *occurrence.clipPlayer, *occurrence.clipBindingIndex, input, stage, occurrence);
            if (occurrence.blendTree.has_value())
                return TraverseTree(state, occurrence, input, stage);
            return {};
        }
    }  // namespace

    /** @copydoc TraverseGraphStateBranch */
    GraphInstanceOutcome TraverseGraphStateBranch(GraphInstanceState &state, const std::uint32_t machine, const GraphStateBranch &branch,
                                                  const std::uint8_t lane, const GraphEvaluationInput &input, const bool stageCursors) {
        for (const auto &occurrence : GraphStateBranchOccurrences(state, branch, lane)) {
            auto localInput = input;
            if (!ActiveBeforeControl(state, machine, occurrence, localInput))
                continue;
            if (const auto result = TraverseOccurrence(state, occurrence, localInput, stageCursors); result.HasError())
                return result;
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
