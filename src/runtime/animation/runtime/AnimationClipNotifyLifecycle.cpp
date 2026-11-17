#include "AnimationGraphInstanceInternal.h"

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Action terminal outcomes are explicit lifecycle closures, never fabricated endpoint travel. */
        AnimationNotifyReason Reason(const GraphActionCompletionReason reason) noexcept {
            if (reason == GraphActionCompletionReason::Completed)
                return AnimationNotifyReason::PlaybackEnd;
            if (reason == GraphActionCompletionReason::Interrupted)
                return AnimationNotifyReason::Interrupted;
            return AnimationNotifyReason::Cancelled;
        }
    }  // namespace

    /** @copydoc CloseActionClipNotifies */
    void CloseActionClipNotifies(GraphInstanceState &state, const std::uint32_t index, const GraphActionCompletionReason reason) noexcept {
        const auto action = state.actions.candidateRecords[index].action;
        const auto slot = state.actions.definitions[action].slot;
        if (state.actions.candidateSlots[slot].active != index)
            return;
        CloseClipNotifyPlayer(state, ActionPlayer(state, slot), Reason(reason));
    }

    /** @copydoc EnterActionClipNotifyLoop */
    void EnterActionClipNotifyLoop(GraphInstanceState &state, const std::uint32_t player,
                                   const AnimationTraversalDirection direction) noexcept {
        auto &work = state.notifies;
        if (work.candidate.empty())
            return;
        auto &owner = work.candidate[player];
        if (!owner.initialized || owner.travelled || owner.originReason != AnimationNotifyReason::Loop)
            return;
        ClipNotifyOrigin(state, player, owner.sample, direction, true, AnimationNotifyReason::Loop, true);
        owner.travelled = true;
    }
}  // namespace Horo::Animation::Detail
