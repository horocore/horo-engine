#include "AnimationGraphInstanceInternal.h"

#include <algorithm>

namespace Horo::Animation::Detail {
    /** @copydoc CommitActions */
    void CommitActions(GraphInstanceState &state, const std::uint32_t publication, const AnimationTickId tick) noexcept {
        auto &work = state.actions;
        work.records.swap(work.candidateRecords);
        work.slots.swap(work.candidateSlots);
        work.admissions.clear();
        std::ranges::copy(work.overlay, work.committedOverlay.begin());
        std::ranges::copy(work.frozen, work.committedFrozen.begin());
        work.completionCounts[publication] = 0;
        for (auto &record : work.records) {
            record.cancelIntent = false;
            // A queued request has not yet transferred release intent to its actual slot/loop owner.
            if (record.observation.phase != GraphActionPhase::Queued && record.observation.phase != GraphActionPhase::Pending)
                record.releaseIntent = false;
            if (record.observation.handle.IsValid() && record.observation.phase == GraphActionPhase::Terminal &&
                record.observation.terminalTick == tick)
                work.completions[publication][work.completionCounts[publication]++] = record.observation;
        }
        for (std::uint32_t i = 0; i < work.slots.size(); ++i) {
            const auto &slot = work.slots[i];
            auto &output = work.published[publication][i];
            output = {.slot = state.program->ActionSlots()[i].id, .queued = slot.queued};
            if (slot.active == NoActionIndex)
                continue;
            const auto &record = work.records[slot.active];
            output.active = record.observation.handle;
            output.action = record.observation.action;
            output.section = record.observation.section;
            output.sampleTime = slot.sampleTime;
            output.weight = slot.weight;
        }
    }

    /** @copydoc ShutdownActions */
    void ShutdownActions(GraphInstanceState &state) noexcept {
        auto &work = state.actions;
        for (auto &record : work.records) {
            if (!record.observation.handle.IsValid() || record.observation.phase == GraphActionPhase::Terminal)
                continue;
            record.observation.phase = GraphActionPhase::Terminal;
            record.observation.reason = GraphActionCompletionReason::Shutdown;
            record.observation.terminalTick = {};
            record.observation.weight = 0;
        }
        for (auto &slot : work.slots)
            slot = {};
        work.admissions.clear();
    }
}  // namespace Horo::Animation::Detail
