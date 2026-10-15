#include "Horo/AI/AIRuntimeCapability.h"

#include "AISceneRuntimeDetail.h"

#include <limits>

namespace Horo::AI {
    namespace {
        /** @brief Checks agent residency and enabled state before exposing owned mutable storage. */
        Result<Detail::AgentRuntimeState *> RequireAgent(Detail::AiSceneRuntimeState &state, AgentHandle agent) {
            auto *slot = Detail::FindAgent(state, agent);
            if (!slot || slot->record.state != AiAgentActivationState::Active)
                return Result<Detail::AgentRuntimeState *>::Failure(MakeError(AIErrors::HandleInvalid));
            return Result<Detail::AgentRuntimeState *>::Success(slot);
        }

        /** @brief Requires exact task identity; a newer execution never aliases an older request. */
        Result<AiTaskLifecycle *> RequireTask(Detail::AiSceneRuntimeState &state, AgentHandle agent, TaskHandle task) {
            auto slot = RequireAgent(state, agent);
            if (slot.HasError())
                return Result<AiTaskLifecycle *>::Failure(slot.ErrorValue());
            auto *lifecycle = slot.Value()->task.get();
            if (!lifecycle || !lifecycle->Context() || lifecycle->Context()->task != task)
                return Result<AiTaskLifecycle *>::Failure(MakeError(AIErrors::HandleInvalid));
            return Result<AiTaskLifecycle *>::Success(lifecycle);
        }
    }  // namespace

    /** @copydoc AiRuntimeCapability::Lock */
    Result<std::shared_ptr<Detail::AiSceneRuntimeState>> AiRuntimeCapability::Lock() const {
        if (std::this_thread::get_id() != owner_)
            return Result<std::shared_ptr<Detail::AiSceneRuntimeState>>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        auto state = state_.lock();
        if (!state || state->owner != std::this_thread::get_id() || state->closed)
            return Result<std::shared_ptr<Detail::AiSceneRuntimeState>>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        return Result<std::shared_ptr<Detail::AiSceneRuntimeState>>::Success(std::move(state));
    }

    /** @copydoc AiRuntimeCapability::ValidateAtSafePoint */
    Result<void> AiRuntimeCapability::ValidateAtSafePoint() const {
        if (auto state = Lock(); state.HasError())
            return Result<void>::Failure(state.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc AiRuntimeCapability::ResolveAgent */
    Result<AgentHandle> AiRuntimeCapability::ResolveAgent(AgentId agent) const {
        auto state = Lock();
        if (state.HasError())
            return Result<AgentHandle>::Failure(state.ErrorValue());
        for (const auto &slot : state.Value()->agents)
            if (!slot.retired && slot.record.state == AiAgentActivationState::Active && slot.record.agent.agent == agent)
                return Result<AgentHandle>::Success(slot.record.handle);
        return Result<AgentHandle>::Failure(MakeError(AIErrors::HandleInvalid));
    }

    /** @copydoc AiRuntimeCapability::StartTask */
    Result<TaskHandle> AiRuntimeCapability::StartTask(AgentHandle agent, const AiCapabilityTaskDescriptor &task,
                                                      CancellationToken cancellation,
                                                      const Telemetry::OperationContext &diagnostics) const {
        auto state = Lock();
        if (state.HasError())
            return Result<TaskHandle>::Failure(state.ErrorValue());
        auto slot = RequireAgent(*state.Value(), agent);
        if (slot.HasError())
            return Result<TaskHandle>::Failure(slot.ErrorValue());
        if (task.blackboard && (!slot.Value()->schema || slot.Value()->schema->Identity() != *task.blackboard))
            return Result<TaskHandle>::Failure(MakeError(AIErrors::CapabilityUnavailable));
        try {
            return Detail::StartOwnedTask(*state.Value(), agent, task.task, std::move(cancellation), diagnostics);
        } catch (const std::bad_alloc &) {
            return Result<TaskHandle>::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        }
    }

    /** @copydoc AiRuntimeCapability::InspectTask */
    Result<AiCapabilityTaskSnapshot> AiRuntimeCapability::InspectTask(AgentHandle agent, TaskHandle task) const {
        auto state = Lock();
        if (state.HasError())
            return Result<AiCapabilityTaskSnapshot>::Failure(state.ErrorValue());
        auto lifecycle = RequireTask(*state.Value(), agent, task);
        if (lifecycle.HasError())
            return Result<AiCapabilityTaskSnapshot>::Failure(lifecycle.ErrorValue());
        if (state.Value()->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<AiCapabilityTaskSnapshot>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        auto resumed = lifecycle.Value()->PrepareResume(AiTaskResumeReason::FixedTick, agent);
        if (resumed.HasError())
            return Result<AiCapabilityTaskSnapshot>::Failure(resumed.ErrorValue());
        if (resumed.Value() == AiTaskResumeDisposition::BecameTerminal)
            ++state.Value()->revision;
        const auto *terminal = lifecycle.Value()->TerminalResult();
        return Result<AiCapabilityTaskSnapshot>::Success({task, lifecycle.Value()->Context()->taskDefinition, lifecycle.Value()->State(),
                                                          terminal ? std::optional{*terminal} : std::nullopt,
                                                          lifecycle.Value()->Context()->diagnostics});
    }

    /** @copydoc AiRuntimeCapability::CancelTask */
    Result<AiTaskTransitionDisposition> AiRuntimeCapability::CancelTask(AgentHandle agent, TaskHandle task) const {
        auto state = Lock();
        if (state.HasError())
            return Result<AiTaskTransitionDisposition>::Failure(state.ErrorValue());
        auto lifecycle = RequireTask(*state.Value(), agent, task);
        if (lifecycle.HasError())
            return Result<AiTaskTransitionDisposition>::Failure(lifecycle.ErrorValue());
        if (state.Value()->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<AiTaskTransitionDisposition>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        auto changed = lifecycle.Value()->RequestCancellation(AiTaskCancellationReason::Requested);
        if (changed.HasValue() && changed.Value() == AiTaskTransitionDisposition::Applied)
            ++state.Value()->revision;
        return changed;
    }

    /** @copydoc AiRuntimeCapability::CompleteTask */
    Result<AiTaskTransitionDisposition> AiRuntimeCapability::CompleteTask(AgentHandle agent, TaskHandle task) const {
        auto state = Lock();
        if (state.HasError())
            return Result<AiTaskTransitionDisposition>::Failure(state.ErrorValue());
        auto lifecycle = RequireTask(*state.Value(), agent, task);
        if (lifecycle.HasError())
            return Result<AiTaskTransitionDisposition>::Failure(lifecycle.ErrorValue());
        if (state.Value()->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<AiTaskTransitionDisposition>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        auto changed = lifecycle.Value()->CompleteSuccess(agent);
        if (changed.HasValue() && changed.Value() == AiTaskTransitionDisposition::Applied)
            ++state.Value()->revision;
        return changed;
    }

    /** @copydoc AiRuntimeCapability::ReleaseTask */
    Result<void> AiRuntimeCapability::ReleaseTask(AgentHandle agent, TaskHandle task) const {
        auto state = Lock();
        if (state.HasError())
            return Result<void>::Failure(state.ErrorValue());
        auto lifecycle = RequireTask(*state.Value(), agent, task);
        if (lifecycle.HasError())
            return Result<void>::Failure(lifecycle.ErrorValue());
        if (state.Value()->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        if (!lifecycle.Value()->TerminalResult())
            return Result<void>::Failure(MakeError(AIErrors::TaskTransitionInvalid));
        auto slot = RequireAgent(*state.Value(), agent);
        if (slot.Value()->blackboard) {
            auto removed = slot.Value()->blackboard->CancelTaskObserversAtBlackboardSync(task);
            if (removed.HasError())
                return Result<void>::Failure(removed.ErrorValue());
        }
        if (auto claimed = lifecycle.Value()->ClaimCleanup(); claimed.HasError())
            return Result<void>::Failure(claimed.ErrorValue());
        if (auto cleaned = lifecycle.Value()->CompleteCleanup(); cleaned.HasError())
            return Result<void>::Failure(cleaned.ErrorValue());
        slot.Value()->task.reset();
        slot.Value()->record.hasRunningTask = false;
        ++state.Value()->revision;
        return Result<void>::Success();
    }

    /** @copydoc AiRuntimeCapability::ReadBlackboard */
    Result<std::optional<BlackboardValue>> AiRuntimeCapability::ReadBlackboard(AgentHandle agent, BlackboardKeyId key) const {
        auto state = Lock();
        if (state.HasError())
            return Result<std::optional<BlackboardValue>>::Failure(state.ErrorValue());
        auto slot = RequireAgent(*state.Value(), agent);
        if (slot.HasError())
            return Result<std::optional<BlackboardValue>>::Failure(slot.ErrorValue());
        if (!slot.Value()->blackboard)
            return Result<std::optional<BlackboardValue>>::Failure(MakeError(AIErrors::CapabilityUnavailable));
        return slot.Value()->blackboard->ReadAtSafePoint(key);
    }

    /** @copydoc AiRuntimeCapability::BeginBlackboardWrite */
    Result<BlackboardWriteBatch> AiRuntimeCapability::BeginBlackboardWrite(AgentHandle agent) const {
        auto state = Lock();
        if (state.HasError())
            return Result<BlackboardWriteBatch>::Failure(state.ErrorValue());
        auto slot = RequireAgent(*state.Value(), agent);
        if (slot.HasError())
            return Result<BlackboardWriteBatch>::Failure(slot.ErrorValue());
        if (!slot.Value()->blackboard)
            return Result<BlackboardWriteBatch>::Failure(MakeError(AIErrors::CapabilityUnavailable));
        return slot.Value()->blackboard->BeginWriteBatch();
    }

    /** @copydoc AiRuntimeCapability::CommitBlackboardWrite */
    Result<BlackboardCommitResult> AiRuntimeCapability::CommitBlackboardWrite(AgentHandle agent, BlackboardWriteBatch batch) const {
        auto state = Lock();
        if (state.HasError())
            return Result<BlackboardCommitResult>::Failure(state.ErrorValue());
        auto slot = RequireAgent(*state.Value(), agent);
        if (slot.HasError())
            return Result<BlackboardCommitResult>::Failure(slot.ErrorValue());
        if (!slot.Value()->blackboard)
            return Result<BlackboardCommitResult>::Failure(MakeError(AIErrors::CapabilityUnavailable));
        if (state.Value()->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<BlackboardCommitResult>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        auto committed = slot.Value()->blackboard->CommitAtBlackboardSync(std::move(batch));
        if (committed.HasValue() && committed.Value().changedKeyCount != 0)
            ++state.Value()->revision;
        return committed;
    }
}  // namespace Horo::AI
