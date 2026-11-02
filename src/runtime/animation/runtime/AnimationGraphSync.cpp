#include "AnimationGraphSyncMapping.h"

#include <algorithm>
#include <array>
#include <limits>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Resolves immutable authored policy through the actual retained program occurrence. */
        const GraphBlendTreeNode &Tree(const GraphInstanceState &state, const GraphBlendTreeWork &work) noexcept {
            const auto &occurrence = state.program->Occurrences()[work.occurrence];
            return std::get<GraphBlendTreeNode>(
                state.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex].payload);
        }

        /** @brief Borrows one compiler-owned exact player/clip binding. */
        const GraphBlendSampleBinding &Binding(const GraphInstanceState &state, const GraphBlendMemberWork &member) noexcept {
            return state.program->Occurrences()[state.blends.trees[member.tree].occurrence].blendSamples[member.sample];
        }

        /** @brief A frozen transition source keeps its original cursors and cannot lead an advancing group. */
        bool FrozenSource(const GraphInstanceState &state, const GraphInstructionOccurrence &occurrence) noexcept {
            for (const auto &guard : occurrence.guards) {
                const auto &machine = state.candidate.machines[guard.machine];
                if (machine.runtime.frozenSource && guard.lane == machine.sourceLane)
                    return true;
            }
            return false;
        }

        /** @brief Uses canonical staged transition contributions solely for leader selection; tree pose weights remain normalized. */
        double StateContribution(const GraphInstanceState &state, const GraphInstructionOccurrence &occurrence) noexcept {
            double contribution = 1.0;
            for (const auto &guard : occurrence.guards) {
                const auto &machine = state.candidate.machines[guard.machine];
                if (!machine.runtime.destination)
                    continue;
                contribution *= guard.lane == machine.sourceLane ? 1.0 - machine.outcome.weight : machine.outcome.weight;
            }
            return contribution;
        }

        /** @brief Resets all invocation observations while retaining inactive/frozen players' actual sample positions. */
        void CaptureMember(GraphInstanceState &state, const GraphBlendTreeWork &work, const GraphBlendTreeNode &tree,
                           const std::size_t sample, const float weight) {
            const auto &occurrence = state.program->Occurrences()[work.occurrence];
            const auto &binding = occurrence.blendSamples[sample];
            auto &member = state.blends.members[binding.member];
            member.leaderWeight = static_cast<double>(weight) * StateContribution(state, occurrence);
            const auto cursor = state.candidate.players[binding.player].cursor;
            const auto &descriptor = state.clips[binding.clip]->Data().descriptor;
            const AnimationTime time{descriptor.wrapMode == AnimationWrapMode::PingPong && cursor.phase.ticks > descriptor.duration.ticks
                                         ? 2 * descriptor.duration.ticks - cursor.phase.ticks
                                         : cursor.phase.ticks};
            member.before = cursor;
            member.delta = {};
            member.traversal = {.cursor = cursor, .sampleTime = time, .direction = AnimationTraversalDirection::Held};
            member.observation = {.occurrence = work.occurrence,
                                  .sample = tree.samples[sample].id,
                                  .group = tree.group,
                                  .weight = weight,
                                  .sampleTime = time};
        }

        /** @brief Captures active geometry in fixed stack scratch, with no cursor mutation or retained invocation spans. */
        GraphInstanceOutcome Weights(GraphInstanceState &state, const GraphEvaluationInput &input) {
            for (auto &work : state.blends.trees) {
                const auto &occurrence = state.program->Occurrences()[work.occurrence];
                auto local = input;
                work.active = StateBranchInput(state, occurrence, local);
                work.delta = local.delta;
                work.rate = local.rate;
                work.frozenSource = FrozenSource(state, occurrence);
                work.active = work.active && !work.frozenSource;
                const auto &tree = Tree(state, work);
                std::array<float, AnimationGraphHardLimits::BlendSamplesPerTree> weights{};
                if (work.active) {
                    const auto x = std::get<float>(state.candidate.parameters[occurrence.blendParameters[0]]);
                    const auto y = tree.dimension == GraphBlendDimension::Two
                                       ? std::get<float>(state.candidate.parameters[occurrence.blendParameters[1]])
                                       : 0.0F;
                    if (const auto result = ComputeGraphBlendWeights(tree, x, y, std::span{weights}.first(tree.samples.size()));
                        result.HasError())
                        return result;
                }
                for (std::size_t i = 0; i < tree.samples.size(); ++i)
                    CaptureMember(state, work, tree, i, weights[i]);
            }
            return {};
        }

        /** @brief Advances exactly the member's original candidate cursor through the shared clip traversal authority. */
        GraphInstanceOutcome Advance(GraphInstanceState &state, GraphBlendMemberWork &member, const AnimationTimeDelta delta) {
            const auto &binding = Binding(state, member);
            auto &player = state.candidate.players[binding.player];
            const auto traversal = state.clips[binding.clip]->TraversePrepared(player.cursor, delta, state.clipLimits);
            if (traversal.HasError())
                return {GraphInstanceFailure::ClipFailure, traversal.ErrorValue()};
            member.before = player.cursor;
            member.delta = delta;
            member.traversal = traversal.Value();
            member.observation.sampleTime = traversal.Value().sampleTime;
            member.observation.advance = delta;
            member.observation.boundaryCrossings = traversal.Value().boundaryCrossings;
            member.observation.direction = traversal.Value().direction;
            player.cursor = traversal.Value().cursor;
            return {};
        }

        /** @brief Uses the same fixed-rate residual owned by this actual player; no group clock is introduced. */
        GraphInstanceOutcome Independent(GraphInstanceState &state, GraphBlendMemberWork &member) {
            const auto &tree = state.blends.trees[member.tree];
            GraphEvaluationInput input{.delta = tree.delta, .rate = tree.rate};
            const auto delta = ScaleGraphDelta(input, state.candidate.players[Binding(state, member).player]);
            if (delta.HasError())
                return {GraphInstanceFailure::ClipFailure, delta.ErrorValue()};
            return Advance(state, member, delta.Value());
        }

        /** @brief Stable occurrence/member order breaks equal weights; explicit followers cannot displace an eligible leader. */
        std::uint32_t Leader(const GraphInstanceState &state, const GraphSyncGroupWork &group) noexcept {
            auto selected = std::numeric_limits<std::uint32_t>::max();
            bool eligible = false;
            for (const auto index : group.members) {
                const auto &member = state.blends.members[index];
                if (member.observation.weight <= 0.0F || state.blends.trees[member.tree].frozenSource)
                    continue;
                const auto canLead = Tree(state, state.blends.trees[member.tree]).samples[member.sample].canLead;
                if (eligible && !canLead)
                    continue;
                if (selected == std::numeric_limits<std::uint32_t>::max() || (canLead && !eligible) ||
                    member.leaderWeight > state.blends.members[selected].leaderWeight) {
                    selected = index;
                    eligible = canLead;
                }
            }
            return selected;
        }

        /** @brief Canonical positive phase of an unwrapped marker interval. */
        std::int64_t Modulo(const std::int64_t value, const std::int64_t duration) noexcept {
            const auto remainder = value % duration;
            return remainder < 0 ? remainder + duration : remainder;
        }

        /** @brief Adapts follower travel to the leader's exact cycle and piecewise marker interval, including reverse/multiple wraps. */
        GraphInstanceOutcome Follow(GraphInstanceState &state, GraphBlendMemberWork &member, const GraphBlendMemberWork &leader,
                                    const AnimationClipCursor leaderBefore, const GraphSyncMode mode) {
            const auto &source = state.clips[Binding(state, leader).clip]->Data();
            const auto &target = state.clips[Binding(state, member).clip]->Data();
            bool fallback = false;
            const auto oldMapped = MapGraphSyncPhase(source, target, leaderBefore.phase.ticks, mode == GraphSyncMode::Markers, fallback);
            const auto newMapped =
                MapGraphSyncPhase(source, target, leader.traversal.sampleTime.ticks, mode == GraphSyncMode::Markers, fallback);
            member.observation.markerFallback = fallback;
            const auto signedCycles =
                (leaderBefore.phase.ticks + leader.delta.ticks - leader.traversal.cursor.phase.ticks) / source.descriptor.duration.ticks;
            auto &player = state.candidate.players[Binding(state, member).player];
            const auto aligned = Modulo(oldMapped, target.descriptor.duration.ticks) - player.cursor.phase.ticks;
            member.observation.alignment = {aligned};
            const AnimationTimeDelta delta{aligned + newMapped - oldMapped + signedCycles * target.descriptor.duration.ticks};
            player.remainder = 0;
            return Advance(state, member, delta);
        }

        /** @brief Alignment relocates the cursor but is never notify travel; only the real follower advance is traversed. */
        GraphInstanceOutcome NotifyMember(GraphInstanceState &state, const GraphBlendMemberWork &member) noexcept {
            const auto &binding = Binding(state, member);
            const auto &clip = *state.clips[binding.clip];
            const auto aligned = clip.TraversePrepared(member.before, member.observation.alignment, state.clipLimits);
            if (aligned.HasError())
                return {GraphInstanceFailure::ClipFailure, aligned.ErrorValue()};
            const auto &descriptor = clip.Data().descriptor;
            const auto &occurrence = state.program->Occurrences()[state.blends.trees[member.tree].occurrence];
            const bool allowed =
                member.observation.eventsEligible && member.observation.weight > 0.0F && ClipNotifyEligible(state, occurrence);
            return StageClipNotifies(state, binding.player, binding.clip, aligned.Value().cursor,
                                     {member.delta.ticks - member.observation.alignment.ticks},
                                     {{0}, {descriptor.duration.ticks}, descriptor.wrapMode}, allowed);
        }

        /** @brief Computes the explicit event filter without consuming or creating any clip event. */
        bool EventsEligible(const GraphSyncEventPolicy policy, const bool leader) noexcept {
            return policy == GraphSyncEventPolicy::AllActive || (policy == GraphSyncEventPolicy::LeaderOnly && leader);
        }

        /** @brief Prepares one disjoint member set; all candidate changes remain invisible until the existing Commit fence. */
        GraphInstanceOutcome Synchronize(GraphInstanceState &state, const GraphSyncGroupWork &group) {
            const auto leaderIndex = Leader(state, group);
            if (leaderIndex == std::numeric_limits<std::uint32_t>::max())
                return {};
            auto &leader = state.blends.members[leaderIndex];
            const auto before = state.candidate.players[Binding(state, leader).player].cursor;
            leader.observation.leader = true;
            if (const auto result = Independent(state, leader); result.HasError())
                return result;
            for (const auto index : group.members) {
                auto &member = state.blends.members[index];
                if (member.observation.weight <= 0.0F || state.blends.trees[member.tree].frozenSource)
                    continue;
                member.observation.eventsEligible = EventsEligible(group.policy.events, index == leaderIndex);
                if (index == leaderIndex)
                    continue;
                const auto result = group.policy.mode == GraphSyncMode::Independent
                                        ? Independent(state, member)
                                        : Follow(state, member, leader, before, group.policy.mode);
                if (result.HasError())
                    return result;
            }
            return {};
        }
    }  // namespace

    /** @copydoc PrepareGraphSync */
    GraphInstanceOutcome PrepareGraphSync(GraphInstanceState &state, const GraphEvaluationInput &input) {
        if (const auto weights = Weights(state, input); weights.HasError())
            return weights;
        for (const auto &group : state.blends.groups) {
            if (input.cancellation.IsCancellationRequested() || state.ownerCancellation.IsCancellationRequested())
                return {GraphInstanceFailure::Cancelled};
            if (const auto result = Synchronize(state, group); result.HasError())
                return result;
        }
        for (const auto &member : state.blends.members) {
            if (!state.blends.trees[member.tree].active)
                continue;
            if (const auto notified = NotifyMember(state, member); notified.HasError())
                return notified;
        }
        return {};
    }
}  // namespace Horo::Animation::Detail
