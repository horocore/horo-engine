#include "AnimationGraphInstanceInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Animation {
    namespace {
        using namespace Detail;

        /** @brief Owner/candidate gates run before mutable record lookup. */
        GraphActionFailure Gate(const GraphInstanceState *state, const bool allowClosed = false,
                                const bool allowCandidate = false) noexcept {
            if (!state)
                return GraphActionFailure::Closed;
            if (state->ownerThread != std::this_thread::get_id())
                return GraphActionFailure::ThreadViolation;
            if (state->closed && !allowClosed)
                return GraphActionFailure::Closed;
            if (state->candidate.tick.IsValid() && !allowCandidate)
                return GraphActionFailure::EvaluationPending;
            return GraphActionFailure::None;
        }

        /** @brief Exact issued identity prevents stale released records and foreign
         * instance generations from aliasing. */
        std::uint32_t Record(const GraphInstanceState &state, const GraphActionHandle handle) noexcept {
            if (!handle.IsValid() || handle.instance != state.identity)
                return NoActionIndex;
            for (std::uint32_t i = 0; i < state.actions.records.size(); ++i)
                if (state.actions.records[i].observation.handle == handle)
                    return i;
            return NoActionIndex;
        }

        /** @brief Resolves exact immutable action/start-section before reserving any
         * request table entry. */
        bool Request(const GraphInstanceState &state, const GraphActionRequest &request, std::uint32_t &action,
                     std::uint32_t &section) noexcept {
            const auto actions = state.program->Actions();
            const auto found = std::ranges::lower_bound(actions, request.action, {}, &GraphActionDefinition::id);
            if (found == actions.end() || found->id != request.action)
                return false;
            action = static_cast<std::uint32_t>(found - actions.begin());
            const auto selected = request.startSection.IsValid() ? request.startSection : found->sequence.entry;
            const auto start = std::ranges::lower_bound(found->sequence.sections, selected, {}, &GraphActionSection::id);
            if (start == found->sequence.sections.end() || start->id != selected)
                return false;
            section = static_cast<std::uint32_t>(start - found->sequence.sections.begin());
            return true;
        }

        /** @brief Reserves only an unregistered record; terminal records remain charged
         * until explicit result release. */
        std::uint32_t Free(const GraphInstanceState &state) noexcept {
            for (std::uint32_t i = 0; i < state.actions.records.size(); ++i)
                if (!state.actions.records[i].observation.handle.IsValid())
                    return i;
            return NoActionIndex;
        }
    }  // namespace

    /** @copydoc AnimationGraphInstance::QueueAction */
    GraphActionCommandOutcome AnimationGraphInstance::QueueAction(const GraphActionRequest &request) {
        if (const auto gate = Gate(state_.get()); gate != GraphActionFailure::None)
            return {gate};
        if (!request.action.IsValid() || request.queue >= GraphActionQueuePolicy::Count)
            return {GraphActionFailure::Malformed};
        std::uint32_t action = 0, section = 0;
        if (!Request(*state_, request, action, section))
            return {GraphActionFailure::BindingMismatch};
        auto &work = state_->actions;
        const auto index = Free(*state_);
        if (index == NoActionIndex || work.nextOrdinal == 0 || work.admissions.size() >= work.limits.transitionsPerTick)
            return {GraphActionFailure::CapacityExceeded};
        const GraphActionHandle handle{state_->identity, work.nextOrdinal};
        auto &record = work.records[index];
        record = {};
        record.action = action;
        record.start = section;
        record.queue = request.queue;
        record.cancellation = request.cancellation;
        const auto &definition = state_->program->Actions()[action];
        record.observation = {.handle = handle,
                              .action = definition.id,
                              .slot = definition.slot,
                              .section = definition.sequence.sections[section].id};
        work.admissions.push_back(index);
        work.nextOrdinal = work.nextOrdinal == std::numeric_limits<std::uint64_t>::max() ? 0 : work.nextOrdinal + 1;
        return {GraphActionFailure::None, handle};
    }

    /** @copydoc AnimationGraphInstance::CancelAction */
    GraphActionCommandOutcome AnimationGraphInstance::CancelAction(const GraphActionHandle handle, const GraphActionStopPolicy policy) {
        if (const auto gate = Gate(state_.get()); gate != GraphActionFailure::None)
            return {gate};
        if (policy >= GraphActionStopPolicy::Count)
            return {GraphActionFailure::Malformed};
        const auto index = Record(*state_, handle);
        if (index == NoActionIndex)
            return {GraphActionFailure::HandleStale};
        auto &record = state_->actions.records[index];
        if (record.observation.phase == GraphActionPhase::Terminal)
            return {GraphActionFailure::None, handle};
        if (!record.cancelIntent || policy == GraphActionStopPolicy::Cut)
            record.stop = policy;
        record.cancelIntent = true;
        return {GraphActionFailure::None, handle};
    }

    /** @copydoc AnimationGraphInstance::ReleaseActionLoop */
    GraphActionCommandOutcome AnimationGraphInstance::ReleaseActionLoop(const GraphActionHandle handle) {
        if (const auto gate = Gate(state_.get()); gate != GraphActionFailure::None)
            return {gate};
        const auto index = Record(*state_, handle);
        if (index == NoActionIndex)
            return {GraphActionFailure::HandleStale};
        auto &record = state_->actions.records[index];
        if (record.observation.phase == GraphActionPhase::Terminal)
            return {GraphActionFailure::NotTerminal};
        if (state_->program->Actions()[record.action].sequence.loops.empty())
            return {GraphActionFailure::BindingMismatch};
        record.releaseIntent = true;
        return {GraphActionFailure::None, handle};
    }

    /** @copydoc AnimationGraphInstance::ReleaseActionResult */
    GraphActionCommandOutcome AnimationGraphInstance::ReleaseActionResult(const GraphActionHandle handle) {
        if (const auto gate = Gate(state_.get(), true); gate != GraphActionFailure::None)
            return {gate};
        const auto index = Record(*state_, handle);
        if (index == NoActionIndex)
            return {GraphActionFailure::HandleStale};
        auto &record = state_->actions.records[index];
        if (record.observation.phase != GraphActionPhase::Terminal)
            return {GraphActionFailure::NotTerminal};
        record = {};
        return {GraphActionFailure::None, handle};
    }

    /** @copydoc AnimationGraphInstance::QueryAction */
    GraphActionQueryOutcome AnimationGraphInstance::QueryAction(const GraphActionHandle handle) const {
        if (const auto gate = Gate(state_.get(), true, true); gate != GraphActionFailure::None)
            return {gate};
        const auto index = Record(*state_, handle);
        if (index == NoActionIndex)
            return {GraphActionFailure::HandleStale};
        return {GraphActionFailure::None, state_->actions.records[index].observation};
    }
}  // namespace Horo::Animation

namespace Horo::Animation::Detail {
    /** @copydoc SpendActionTransition */
    GraphInstanceOutcome SpendActionTransition(GraphInstanceState &state) noexcept {
        if (state.actions.transitions >= state.actions.limits.transitionsPerTick)
            return {GraphInstanceFailure::LimitExceeded};
        ++state.actions.transitions;
        return {};
    }

    /** @copydoc FinishAction */
    void FinishAction(GraphInstanceState &state, const std::uint32_t index, const GraphActionCompletionReason reason,
                      const AnimationTickId tick) noexcept {
        auto &record = state.actions.candidateRecords[index];
        record.observation.phase = GraphActionPhase::Terminal;
        record.observation.reason = reason;
        record.observation.terminalTick = tick;
        record.observation.weight = 0;
        record.cancelIntent = false;
        record.releaseIntent = false;
        auto &slot = state.actions.candidateSlots[state.actions.definitions[record.action].slot];
        if (slot.active == index) {
            slot.active = NoActionIndex;
            slot.weight = 0;
            slot.handoffAction = NoActionIndex;
        }
        if (slot.interrupt == index) {
            slot.interrupt = NoActionIndex;
            --slot.queued;
        }
    }

    /** @copydoc StartAction */
    void StartAction(GraphInstanceState &state, const std::uint32_t index, const bool handoff) noexcept {
        auto &work = state.actions;
        auto &record = work.candidateRecords[index];
        const auto &prepared = work.definitions[record.action];
        auto &slot = work.candidateSlots[prepared.slot];
        const auto &definition = state.program->Actions()[record.action];
        slot.active = index;
        slot.section = record.start;
        slot.loopPasses = 0;
        slot.blendElapsed = 0;
        slot.outElapsed = 0;
        slot.exitReason = GraphActionCompletionReason::None;
        slot.exitLoop = record.releaseIntent;
        slot.directionPending = true;
        slot.sampleTime = prepared.sections[slot.section].region.begin;
        slot.weight = definition.blendIn.ticks == 0 ? 1.0F : 0.0F;
        if (!handoff)
            slot.handoffAction = NoActionIndex;
        state.candidate.players[ActionPlayer(state, prepared.slot)].cursor = {};
        record.observation.phase = GraphActionPhase::Playing;
        record.observation.section = definition.sequence.sections[slot.section].id;
        record.observation.sampleTime = slot.sampleTime;
        record.observation.weight = slot.weight;
    }

    /** @copydoc StopAction */
    void StopAction(GraphInstanceState &state, const std::uint32_t index, const GraphActionCompletionReason reason,
                    const GraphActionStopPolicy policy, const AnimationTickId tick) noexcept {
        auto &record = state.actions.candidateRecords[index];
        const auto &definition = state.program->Actions()[record.action];
        if (policy == GraphActionStopPolicy::Cut || definition.blendOut.ticks == 0) {
            FinishAction(state, index, reason, tick);
            return;
        }
        auto &slot = state.actions.candidateSlots[state.actions.definitions[record.action].slot];
        if (record.observation.phase != GraphActionPhase::BlendingOut) {
            slot.outElapsed = 0;
            slot.outStart = slot.weight;
        }
        record.observation.phase = GraphActionPhase::BlendingOut;
        slot.exitReason = reason;
    }

    /** @copydoc InterruptAction */
    void InterruptAction(GraphInstanceState &state, const std::uint32_t index, const AnimationTickId tick) noexcept {
        auto &work = state.actions;
        const auto slotIndex = work.definitions[work.candidateRecords[index].action].slot;
        auto &slot = work.candidateSlots[slotIndex];
        bool handoff = false;
        if (slot.active != NoActionIndex) {
            const auto &old = work.candidateRecords[slot.active];
            const auto oldAction = old.action, oldSection = slot.section;
            const auto oldWeight = slot.weight;
            const auto joints = state.program->SkeletonBinding().joints;
            const auto offset = static_cast<std::size_t>(slotIndex) * joints;
            std::copy_n(work.committedOverlay.begin() + offset, joints, work.frozen.begin() + offset);
            FinishAction(state, slot.active, GraphActionCompletionReason::Interrupted, tick);
            slot.handoffAction = oldAction;
            slot.handoffSection = oldSection;
            slot.handoffElapsed = 0;
            slot.handoffWeight = oldWeight;
            handoff = true;
        }
        StartAction(state, index, handoff);
    }

    namespace {
        /** @brief Bounded FIFO insertion links existing admitted records, never
         * allocates a queue node. */
        bool Enqueue(GraphInstanceState &state, const std::uint32_t index) noexcept {
            auto &work = state.actions;
            auto &record = work.candidateRecords[index];
            auto &slot = work.candidateSlots[work.definitions[record.action].slot];
            if (slot.queued >= work.limits.queuedPerSlot)
                return false;
            record.next = NoActionIndex;
            record.observation.phase = GraphActionPhase::Queued;
            if (slot.tail == NoActionIndex)
                slot.head = index;
            else
                work.candidateRecords[slot.tail].next = index;
            slot.tail = index;
            ++slot.queued;
            return true;
        }

        /** @brief Explicit request policy respects the active definition's interruption
         * contract. */
        /** @brief Applies only the active definition's closed interruption policy. */
        void AdmitInterrupt(GraphInstanceState &state, const std::uint32_t index, const AnimationTickId tick) noexcept {
            auto &work = state.actions;
            auto &record = work.candidateRecords[index];
            auto &slot = work.candidateSlots[work.definitions[record.action].slot];
            const auto policy = state.program->Actions()[work.candidateRecords[slot.active].action].interruption;
            if (policy == GraphActionInterruption::Immediate) {
                InterruptAction(state, index, tick);
                return;
            }
            if (policy == GraphActionInterruption::AtSectionBoundary && slot.interrupt == NoActionIndex &&
                slot.queued < work.limits.queuedPerSlot) {
                slot.interrupt = index;
                ++slot.queued;
                record.observation.phase = GraphActionPhase::Queued;
                return;
            }
            FinishAction(state, index, GraphActionCompletionReason::Rejected, tick);
        }

        /** @brief Ordinary FIFO work makes a slot busy; only explicit interrupt priority may bypass an ordinary waiting head. */
        bool DirectStart(const ActionSlotState &slot, const GraphActionQueuePolicy policy) noexcept {
            return slot.active == NoActionIndex && slot.interrupt == NoActionIndex &&
                   (slot.head == NoActionIndex || policy == GraphActionQueuePolicy::Interrupt);
        }

        void Admit(GraphInstanceState &state, const std::uint32_t index, const AnimationTickId tick) noexcept {
            auto &work = state.actions;
            auto &record = work.candidateRecords[index];
            auto &slot = work.candidateSlots[work.definitions[record.action].slot];
            if (record.cancelIntent || record.cancellation.IsCancellationRequested()) {
                FinishAction(state, index, GraphActionCompletionReason::Cancelled, tick);
                return;
            }
            if (DirectStart(slot, record.queue)) {
                StartAction(state, index, false);
                return;
            }
            if (record.queue == GraphActionQueuePolicy::Queue) {
                if (!Enqueue(state, index))
                    FinishAction(state, index, GraphActionCompletionReason::Rejected, tick);
                return;
            }
            if (record.queue == GraphActionQueuePolicy::IfIdle || slot.active == NoActionIndex) {
                FinishAction(state, index, GraphActionCompletionReason::Rejected, tick);
                return;
            }
            AdmitInterrupt(state, index, tick);
        }

        /** @brief Removes cancelled queued requests without confusing terminal
         * retention with physical FIFO membership. */
        void CancelQueue(GraphInstanceState &state, ActionSlotState &slot, const AnimationTickId tick) noexcept {
            auto previous = NoActionIndex, current = slot.head;
            while (current != NoActionIndex) {
                auto &record = state.actions.candidateRecords[current];
                const auto next = record.next;
                if (record.cancelIntent || record.cancellation.IsCancellationRequested()) {
                    if (previous == NoActionIndex)
                        slot.head = next;
                    else
                        state.actions.candidateRecords[previous].next = next;
                    if (slot.tail == current)
                        slot.tail = previous;
                    --slot.queued;
                    FinishAction(state, current, GraphActionCompletionReason::Cancelled, tick);
                } else
                    previous = current;
                current = next;
            }
        }

        /** @brief Captured cancellation/loop intents change only candidate owner state
         * until Commit. */
        void Controls(GraphInstanceState &state, ActionSlotState &slot, const AnimationTickId tick) noexcept {
            CancelQueue(state, slot, tick);
            if (slot.interrupt != NoActionIndex) {
                const auto &record = state.actions.candidateRecords[slot.interrupt];
                if (record.cancelIntent || record.cancellation.IsCancellationRequested())
                    FinishAction(state, slot.interrupt, GraphActionCompletionReason::Cancelled, tick);
            }
            if (slot.active == NoActionIndex)
                return;
            auto &record = state.actions.candidateRecords[slot.active];
            slot.exitLoop = slot.exitLoop || record.releaseIntent;
            if (record.cancelIntent || record.cancellation.IsCancellationRequested())
                StopAction(state, slot.active, GraphActionCompletionReason::Cancelled, record.stop, tick);
        }
    }  // namespace

    /** @copydoc AdmitActionCommands */
    GraphInstanceOutcome AdmitActionCommands(GraphInstanceState &state, const AnimationTickId tick) {
        for (const auto index : state.actions.admissions) {
            if (const auto spend = SpendActionTransition(state); spend.HasError())
                return spend;
            Admit(state, index, tick);
        }
        for (auto &slot : state.actions.candidateSlots)
            Controls(state, slot, tick);
        return {};
    }
}  // namespace Horo::Animation::Detail
