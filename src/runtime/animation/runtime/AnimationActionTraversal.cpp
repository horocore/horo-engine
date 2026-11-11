#include "AnimationGraphInstanceInternal.h"

#include <algorithm>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Unsigned magnitude also admits INT64_MIN without signed
         * absolute-value overflow. */
        std::uint64_t Magnitude(const std::int64_t value) noexcept {
            return value < 0 ? 0ULL - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
        }

        /** @brief Existing fixed-rate sign selects the initial region edge; paused
         * requests wait for a nonzero rate. */
        GraphInstanceOutcome Direction(GraphInstanceState &state, const std::uint32_t slotIndex, const GraphPlaybackRate rate) noexcept {
            auto &work = state.actions;
            auto &slot = work.candidateSlots[slotIndex];
            auto &record = work.candidateRecords[slot.active];
            if (const auto &definition = state.program->Actions()[record.action];
                rate.millionths < 0 && definition.reverse == GraphActionReverse::Reject)
                return {GraphInstanceFailure::TypeMismatch};
            if (!slot.directionPending || rate.millionths == 0)
                return {};
            const auto &region = work.definitions[record.action].sections[slot.section].region;
            auto &player = state.candidate.players[ActionPlayer(state, slotIndex)];
            player.cursor = {{rate.millionths < 0 ? region.end.ticks - region.begin.ticks : 0}, false};
            slot.sampleTime = rate.millionths < 0 ? region.end : region.begin;
            slot.directionPending = false;
            record.observation.sampleTime = slot.sampleTime;
            return {};
        }

        /** @brief A real held active request remains visited; sampling alone never creates an activation notify. */
        GraphInstanceOutcome HoldNotifies(GraphInstanceState &state, const std::uint32_t slotIndex, const GraphPlaybackRate rate) noexcept {
            const auto &slot = state.actions.candidateSlots[slotIndex];
            if (slot.active == NoActionIndex)
                return {};
            const auto &record = state.actions.candidateRecords[slot.active];
            const auto &section = state.actions.definitions[record.action].sections[slot.section];
            const auto player = ActionPlayer(state, slotIndex);
            if (const auto staged = StageClipNotifies(state, player, section.clip, state.candidate.players[player].cursor, {},
                                                      section.region, true, record.observation.handle);
                staged.HasError())
                return staged;
            EnterActionClipNotifyLoop(state, player,
                                      rate.millionths < 0 ? AnimationTraversalDirection::Reverse : AnimationTraversalDirection::Forward);
            return {state.notifies.failure};
        }

        /** @brief Direction admission and held publication use the actual resulting action cursor in the same candidate. */
        GraphInstanceOutcome DirectAndHold(GraphInstanceState &state, const std::uint32_t slotIndex,
                                           const GraphPlaybackRate rate) noexcept {
            if (const auto directed = Direction(state, slotIndex, rate); directed.HasError())
                return directed;
            return HoldNotifies(state, slotIndex, rate);
        }

        /** @brief Dequeues an already admitted record into the same slot without
         * another allocation or request issue. */
        GraphInstanceOutcome NextQueued(GraphInstanceState &state, const std::uint32_t slotIndex) noexcept {
            auto &work = state.actions;
            auto &slot = work.candidateSlots[slotIndex];
            auto next = slot.interrupt;
            if (next != NoActionIndex)
                slot.interrupt = NoActionIndex;
            else {
                next = slot.head;
                if (next == NoActionIndex)
                    return {};
                slot.head = work.candidateRecords[next].next;
                if (slot.head == NoActionIndex)
                    slot.tail = NoActionIndex;
            }
            if (const auto spent = SpendActionTransition(state); spent.HasError())
                return spent;
            --slot.queued;
            StartAction(state, next, false);
            return {};
        }

        /** @brief Handoff envelope uses exactly consumed tick time, never a separate
         * wall clock. */
        void Handoff(GraphInstanceState &state, ActionSlotState &slot, const std::int64_t consumed) noexcept {
            if (slot.handoffAction == NoActionIndex)
                return;
            const auto duration = state.program->Actions()[slot.handoffAction].blendOut.ticks;
            slot.handoffElapsed = std::min(duration, slot.handoffElapsed + consumed);
            if (slot.handoffElapsed == duration)
                slot.handoffAction = NoActionIndex;
        }

        /** @brief Blend-in runs alongside actual clip interval consumption; prepared
         * masks remain unchanged. */
        void BlendIn(GraphInstanceState &state, ActionSlotState &slot, const GraphActionDefinition &action,
                     const std::int64_t consumed) noexcept {
            slot.blendElapsed = std::min(action.blendIn.ticks, slot.blendElapsed + consumed);
            slot.weight =
                action.blendIn.ticks == 0 ? 1.0F : ActionCurve(action.curve, static_cast<double>(slot.blendElapsed) / action.blendIn.ticks);
            Handoff(state, slot, consumed);
        }

        /** @brief Consumes a bounded hold-terminal fade, preserving leftover directed
         * time for the next queued action. */
        void FadeOut(GraphInstanceState &state, const std::uint32_t slotIndex, std::int64_t &remaining,
                     const AnimationTickId tick) noexcept {
            auto &work = state.actions;
            auto &slot = work.candidateSlots[slotIndex];
            auto &record = work.candidateRecords[slot.active];
            const auto &definition = state.program->Actions()[record.action];
            const auto consumed = static_cast<std::int64_t>(
                std::min(Magnitude(remaining), static_cast<std::uint64_t>(definition.blendOut.ticks - slot.outElapsed)));
            slot.outElapsed += consumed;
            Handoff(state, slot, consumed);
            slot.weight =
                slot.outStart * (1.0F - ActionCurve(definition.curve, static_cast<double>(slot.outElapsed) / definition.blendOut.ticks));
            record.observation.weight = slot.weight;
            remaining += remaining < 0 ? consumed : -consumed;
            if (slot.outElapsed == definition.blendOut.ticks)
                FinishAction(state, slot.active, slot.exitReason, tick);
        }

        /** @brief An explicit region is the only authorized successor override; exit
         * remains last.next. */
        std::uint32_t Successor(GraphInstanceState &state, ActionSlotState &slot, const PreparedAction &prepared,
                                const GraphActionDefinition &action) noexcept {
            const auto &section = prepared.sections[slot.section];
            if (section.loop == NoActionIndex || prepared.loops[section.loop].last != slot.section)
                return section.next;
            const auto &loop = action.sequence.loops[section.loop];
            bool repeat = false;
            if (loop.mode == GraphActionLoopMode::UntilReleased)
                repeat = !slot.exitLoop;
            else {
                ++slot.loopPasses;
                repeat = !slot.exitLoop && slot.loopPasses < loop.repetitions;
            }
            if (repeat)
                return prepared.loops[section.loop].first;
            slot.loopPasses = 0;
            slot.exitLoop = false;
            return section.next;
        }

        /** @brief Notify ownership follows the exact authored successor selected by the existing action loop controller. */
        void ResetSectionNotifies(GraphInstanceState &state, const std::uint32_t slotIndex, const PreparedAction &prepared,
                                  const std::uint32_t old, const std::uint32_t next, const GraphPlaybackRate rate) noexcept {
            const auto &section = prepared.sections[old];
            const bool repeated =
                section.loop != NoActionIndex && prepared.loops[section.loop].last == old && prepared.loops[section.loop].first == next;
            ResetClipNotifyPlayer(state, ActionPlayer(state, slotIndex),
                                  repeated ? AnimationNotifyReason::Loop : AnimationNotifyReason::SectionChanged,
                                  rate.millionths < 0 ? AnimationTraversalDirection::Reverse : AnimationTraversalDirection::Forward);
        }

        /** @brief Carries a fully consumed section into its actual authored successor
         * or bounded terminal fade. */
        GraphInstanceOutcome Boundary(GraphInstanceState &state, const std::uint32_t slotIndex, const AnimationTickId tick,
                                      const GraphPlaybackRate rate) {
            if (const auto spent = SpendActionTransition(state); spent.HasError())
                return spent;
            auto &work = state.actions;
            auto &slot = work.candidateSlots[slotIndex];
            if (slot.interrupt != NoActionIndex) {
                const auto next = slot.interrupt;
                slot.interrupt = NoActionIndex;
                --slot.queued;
                InterruptAction(state, next, tick);
                return {};
            }
            auto &record = work.candidateRecords[slot.active];
            const auto &prepared = work.definitions[record.action];
            const auto &action = state.program->Actions()[record.action];
            const auto next = Successor(state, slot, prepared, action);
            if (next == NoActionIndex) {
                StopAction(state, slot.active, GraphActionCompletionReason::Completed, GraphActionStopPolicy::BlendOut, tick);
                return {};
            }
            ResetSectionNotifies(state, slotIndex, prepared, slot.section, next, rate);
            slot.section = next;
            slot.directionPending = true;
            state.candidate.players[ActionPlayer(state, slotIndex)].cursor = {};
            record.observation.section = action.sequence.sections[next].id;
            slot.sampleTime = prepared.sections[next].region.begin;
            return {};
        }

        /** @brief Traverses only the consumed regional segment through the same
         * canonical clip kernel. */
        GraphInstanceOutcome Segment(GraphInstanceState &state, const std::uint32_t slotIndex, std::int64_t &remaining) {
            auto &work = state.actions;
            auto &slot = work.candidateSlots[slotIndex];
            auto &record = work.candidateRecords[slot.active];
            const auto &section = work.definitions[record.action].sections[slot.section];
            auto &player = state.candidate.players[ActionPlayer(state, slotIndex)];
            const auto distance = remaining < 0 ? player.cursor.phase.ticks
                                                : section.region.end.ticks - section.region.begin.ticks - player.cursor.phase.ticks;
            const auto amount = static_cast<std::int64_t>(std::min(Magnitude(remaining), static_cast<std::uint64_t>(distance)));
            const auto delta = remaining < 0 ? -amount : amount;
            const auto traversed =
                state.clips[section.clip]->TraverseRegionPrepared(player.cursor, {delta}, section.region, state.clipLimits);
            if (traversed.HasError())
                return {GraphInstanceFailure::ClipFailure, traversed.ErrorValue()};
            work.boundaryCrossings += traversed.Value().boundaryCrossings;
            if (work.boundaryCrossings > state.clipLimits.maximumBoundaryCrossings)
                return {GraphInstanceFailure::ClipFailure,
                        {AnimationClipFailure::LimitExceeded, "The complete action interval exceeds its boundary budget."}};
            if (const auto notified = StageClipNotifies(state, ActionPlayer(state, slotIndex), section.clip, player.cursor, {delta},
                                                        section.region, true, record.observation.handle);
                notified.HasError())
                return notified;
            player.cursor = traversed.Value().cursor;
            slot.sampleTime = traversed.Value().sampleTime;
            BlendIn(state, slot, state.program->Actions()[record.action], amount);
            record.observation.sampleTime = slot.sampleTime;
            record.observation.weight = slot.weight;
            remaining -= delta;
            return {};
        }

        /** @brief A direction change at an already reached section edge performs
         * control handoff without fabricating travel. */
        bool AtEnd(const GraphInstanceState &state, const std::uint32_t slotIndex, const std::int64_t remaining) noexcept {
            const auto &slot = state.actions.candidateSlots[slotIndex];
            const auto &record = state.actions.candidateRecords[slot.active];
            const auto &region = state.actions.definitions[record.action].sections[slot.section].region;
            const auto phase = state.candidate.players[ActionPlayer(state, slotIndex)].cursor.phase.ticks;
            return (remaining < 0 && phase == 0) || (remaining > 0 && phase == region.end.ticks - region.begin.ticks);
        }

        /** @brief One exact active request step; zero traversal still samples its
         * explicit boundary pose. */
        GraphInstanceOutcome Active(GraphInstanceState &state, const std::uint32_t slotIndex, std::int64_t &remaining,
                                    const GraphEvaluationInput &input) {
            const auto &slot = state.actions.candidateSlots[slotIndex];
            if (const auto direction = Direction(state, slotIndex, input.rate); direction.HasError())
                return direction;
            if (state.actions.candidateRecords[slot.active].observation.phase == GraphActionPhase::BlendingOut) {
                FadeOut(state, slotIndex, remaining, input.tick);
                return HoldNotifies(state, slotIndex, input.rate);
            }
            if (remaining == 0)
                return HoldNotifies(state, slotIndex, input.rate);
            if (AtEnd(state, slotIndex, remaining))
                return Boundary(state, slotIndex, input.tick, input.rate);
            if (const auto moved = Segment(state, slotIndex, remaining); moved.HasError())
                return moved;
            if (state.candidate.players[ActionPlayer(state, slotIndex)].cursor.terminal)
                return Boundary(state, slotIndex, input.tick, input.rate);
            return {};
        }

        /** @brief Restores one already-admitted queue head when the slot has no active request. */
        GraphInstanceOutcome EnsureActive(GraphInstanceState &state, const std::uint32_t slot) noexcept {
            if (state.actions.candidateSlots[slot].active != NoActionIndex)
                return {};
            return NextQueued(state, slot);
        }

        /** @brief One bounded section/control iteration reports completion without spinning on an idle slot. */
        GraphInstanceOutcome Iteration(GraphInstanceState &state, const std::uint32_t index, std::int64_t &remaining,
                                       const GraphEvaluationInput &input, bool &done) {
            if (input.cancellation.IsCancellationRequested() || state.ownerCancellation.IsCancellationRequested())
                return {GraphInstanceFailure::Cancelled};
            if (const auto queued = EnsureActive(state, index); queued.HasError())
                return queued;
            const auto &slot = state.actions.candidateSlots[index];
            if (slot.active == NoActionIndex) {
                done = true;
                return {};
            }
            if (const auto advanced = Active(state, index, remaining, input); advanced.HasError())
                return advanced;
            done = remaining == 0;
            if (done && slot.active != NoActionIndex && slot.directionPending)
                return DirectAndHold(state, index, input.rate);
            return {};
        }
    }  // namespace

    /** @copydoc AdvanceActionSlot */
    GraphInstanceOutcome AdvanceActionSlot(GraphInstanceState &state, const std::uint32_t index, const GraphEvaluationInput &input) {
        if (const auto queued = EnsureActive(state, index); queued.HasError())
            return queued;
        if (state.actions.candidateSlots[index].active == NoActionIndex)
            return {};
        const auto scaled = ScaleGraphDelta(input, state.candidate.players[ActionPlayer(state, index)]);
        if (scaled.HasError())
            return {GraphInstanceFailure::ClipFailure, scaled.ErrorValue()};
        auto remaining = scaled.Value().ticks;
        for (;;) {
            bool done = false;
            if (const auto result = Iteration(state, index, remaining, input, done); result.HasError())
                return result;
            if (done)
                return {};
        }
    }
}  // namespace Horo::Animation::Detail
