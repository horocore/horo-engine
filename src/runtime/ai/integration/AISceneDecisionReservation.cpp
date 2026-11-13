#include "AISceneRuntimeDetail.h"

#include <limits>
#include <new>

namespace Horo::AI {
    namespace {
        /** @brief Resolves only an active exact agent after the caller has checked the real owner lane. */
        Result<Detail::AgentRuntimeState *> DecisionAgent(Detail::AiSceneRuntimeState &state, const AgentHandle agent) {
            auto *slot = Detail::FindAgent(state, agent);
            if (!slot || slot->record.state != AiAgentActivationState::Active || !slot->blackboard || !slot->blackboard->IsActive())
                return Result<Detail::AgentRuntimeState *>::Failure(MakeError(AIErrors::HandleInvalid));
            return Result<Detail::AgentRuntimeState *>::Success(slot);
        }
    }  // namespace

    /** @copydoc AiSceneRuntime::ReserveDecisionTaskSlotsAtSafePoint */
    Result<AiDecisionTaskReservation> AiSceneRuntime::ReserveDecisionTaskSlotsAtSafePoint(const AgentHandle agent,
                                                                                          const std::uint32_t count) const {
        using Output = Result<AiDecisionTaskReservation>;
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Output::Failure(owner.ErrorValue());
        auto selected = DecisionAgent(*active_, agent);
        if (selected.HasError())
            return Output::Failure(selected.ErrorValue());
        if (active_->revision == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        constexpr auto End = Horo::Handle<TaskHandleTag>::InvalidIndex;
        if (count > End - active_->nextTaskSlot)
            return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        try {
            auto &slot = *selected.Value();
            AiDecisionTaskReservation reservation{slot.blackboard->Binding(), slot.blackboard->SchemaAtSafePoint(), active_->nextTaskSlot,
                                                  count, slot.cancellation.Token()};
            active_->nextTaskSlot += count;
            ++active_->revision;
            return Output::Success(std::move(reservation));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        }
    }

    /** @copydoc AiSceneRuntime::CaptureDecisionSnapshotAtSafePoint */
    Result<BlackboardSnapshot> AiSceneRuntime::CaptureDecisionSnapshotAtSafePoint(const AgentHandle agent) const {
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Result<BlackboardSnapshot>::Failure(owner.ErrorValue());
        const auto selected = DecisionAgent(*active_, agent);
        if (selected.HasError())
            return Result<BlackboardSnapshot>::Failure(selected.ErrorValue());
        return selected.Value()->blackboard->Snapshot();
    }
}  // namespace Horo::AI
