#include "Horo/AI/AITaskLifecycle.h"

#include "Horo/AI/AIErrors.h"

#include <utility>

namespace Horo::AI {
    namespace {
        [[nodiscard]] bool IsTerminal(const AiTaskState state) noexcept {
            using enum AiTaskState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        [[nodiscard]] bool IsKnown(const AiTaskFailureKind kind) noexcept {
            return kind < AiTaskFailureKind::Count;
        }

        [[nodiscard]] bool IsKnown(const AiTaskCancellationReason reason) noexcept {
            return reason < AiTaskCancellationReason::Count;
        }

        [[nodiscard]] bool IsKnown(const AiTaskResumeReason reason) noexcept {
            return reason < AiTaskResumeReason::Count;
        }

        [[nodiscard]] bool IsValid(const AiTaskOperationContext &context) noexcept {
            return context.taskDefinition.IsValid() && context.task.IsValid() && context.agent.IsValid() &&
                   context.task.incarnation == context.agent.incarnation;
        }

        [[nodiscard]] bool IsValid(const AiTaskFailureDetail &failure) noexcept {
            return IsKnown(failure.kind) && !failure.cause.domain.Value().empty() && !failure.cause.code.Value().empty();
        }

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }
    }  // namespace

    /** @copydoc AiTaskLifecycle::Start */
    Result<AiTaskTransitionDisposition> AiTaskLifecycle::Start(AiTaskOperationContext context) {
        if (state_ != AiTaskState::Idle)
            return Failure<AiTaskTransitionDisposition>(AIErrors::TaskTransitionInvalid);
        if (!IsValid(context))
            return Failure<AiTaskTransitionDisposition>(AIErrors::TaskContextInvalid);

        context_.emplace(std::move(context));
        state_ = AiTaskState::Running;
        cleanupState_ = CleanupState::Pending;
        if (agentGenerationRetired_)
            PublishCancelled(AiTaskCancellationReason::AgentGenerationRetired);
        else if (pendingCancellation_.has_value())
            PublishCancelled(*pendingCancellation_);
        else if (context_->IsCancellationRequested())
            PublishCancelled(AiTaskCancellationReason::ContextCancelled);
        return Result<AiTaskTransitionDisposition>::Success(AiTaskTransitionDisposition::Applied);
    }

    /** @copydoc AiTaskLifecycle::PrepareResume */
    Result<AiTaskResumeDisposition> AiTaskLifecycle::PrepareResume(const AiTaskResumeReason reason, const AgentHandle activeAgent) {
        if (!IsKnown(reason))
            return Failure<AiTaskResumeDisposition>(AIErrors::TaskContextInvalid);
        return CheckExecutionBoundary(activeAgent);
    }

    /** @copydoc AiTaskLifecycle::RequestCancellation */
    Result<AiTaskTransitionDisposition> AiTaskLifecycle::RequestCancellation(const AiTaskCancellationReason reason) {
        using enum AiTaskTransitionDisposition;
        if (!IsKnown(reason))
            return Failure<AiTaskTransitionDisposition>(AIErrors::TaskContextInvalid);
        if (IsTerminal(state_))
            return Result<AiTaskTransitionDisposition>::Success(AlreadyTerminal);
        if (state_ == AiTaskState::Idle) {
            if (pendingCancellation_.has_value())
                return Result<AiTaskTransitionDisposition>::Success(NoChange);
            pendingCancellation_ = reason;
            return Result<AiTaskTransitionDisposition>::Success(Applied);
        }
        PublishCancelled(reason);
        return Result<AiTaskTransitionDisposition>::Success(Applied);
    }

    /** @copydoc AiTaskLifecycle::RetireAgentGeneration */
    AiTaskTransitionDisposition AiTaskLifecycle::RetireAgentGeneration() noexcept {
        using enum AiTaskTransitionDisposition;
        if (IsTerminal(state_))
            return AlreadyTerminal;
        if (agentGenerationRetired_)
            return NoChange;
        agentGenerationRetired_ = true;
        if (state_ == AiTaskState::Running)
            PublishCancelled(AiTaskCancellationReason::AgentGenerationRetired);
        return Applied;
    }

    /** @copydoc AiTaskLifecycle::CompleteSuccess */
    Result<AiTaskTransitionDisposition> AiTaskLifecycle::CompleteSuccess(const AgentHandle activeAgent) {
        return Complete(activeAgent, std::nullopt);
    }

    /** @copydoc AiTaskLifecycle::CompleteFailure */
    Result<AiTaskTransitionDisposition> AiTaskLifecycle::CompleteFailure(const AgentHandle activeAgent, AiTaskFailureDetail failure) {
        return Complete(activeAgent, std::move(failure));
    }

    /** @copydoc AiTaskLifecycle::Complete */
    Result<AiTaskTransitionDisposition> AiTaskLifecycle::Complete(const AgentHandle activeAgent,
                                                                  std::optional<AiTaskFailureDetail> failure) {
        using enum AiTaskTransitionDisposition;
        if (IsTerminal(state_))
            return Result<AiTaskTransitionDisposition>::Success(AlreadyTerminal);
        if (failure.has_value() && !IsValid(*failure))
            return Failure<AiTaskTransitionDisposition>(AIErrors::TaskFailureInvalid);
        const auto boundary = CheckExecutionBoundary(activeAgent);
        if (boundary.HasError())
            return Result<AiTaskTransitionDisposition>::Failure(boundary.ErrorValue());
        if (boundary.Value() != AiTaskResumeDisposition::Ready)
            return Result<AiTaskTransitionDisposition>::Success(AlreadyTerminal);
        state_ = failure.has_value() ? AiTaskState::Failed : AiTaskState::Succeeded;
        terminalResult_.emplace(AiTaskTerminalResult{.state = state_, .failure = std::move(failure)});
        return Result<AiTaskTransitionDisposition>::Success(Applied);
    }

    /** @copydoc AiTaskLifecycle::ClaimCleanup */
    Result<bool> AiTaskLifecycle::ClaimCleanup() {
        if (!IsTerminal(state_))
            return Failure<bool>(AIErrors::TaskTransitionInvalid);
        if (cleanupState_ != CleanupState::Pending)
            return Result<bool>::Success(false);
        cleanupState_ = CleanupState::Claimed;
        return Result<bool>::Success(true);
    }

    /** @copydoc AiTaskLifecycle::CompleteCleanup */
    Result<AiTaskTransitionDisposition> AiTaskLifecycle::CompleteCleanup() {
        if (cleanupState_ == CleanupState::Complete)
            return Result<AiTaskTransitionDisposition>::Success(AiTaskTransitionDisposition::NoChange);
        if (cleanupState_ != CleanupState::Claimed)
            return Failure<AiTaskTransitionDisposition>(AIErrors::TaskTransitionInvalid);
        cleanupState_ = CleanupState::Complete;
        return Result<AiTaskTransitionDisposition>::Success(AiTaskTransitionDisposition::Applied);
    }

    /** @copydoc AiTaskLifecycle::State */
    AiTaskState AiTaskLifecycle::State() const noexcept {
        return state_;
    }

    /** @copydoc AiTaskLifecycle::Context */
    const AiTaskOperationContext *AiTaskLifecycle::Context() const noexcept {
        return context_ ? &*context_ : nullptr;
    }

    /** @copydoc AiTaskLifecycle::TerminalResult */
    const AiTaskTerminalResult *AiTaskLifecycle::TerminalResult() const noexcept {
        return terminalResult_ ? &*terminalResult_ : nullptr;
    }

    /** @copydoc AiTaskLifecycle::IsCleanupComplete */
    bool AiTaskLifecycle::IsCleanupComplete() const noexcept {
        return cleanupState_ == CleanupState::Complete;
    }

    Result<AiTaskResumeDisposition> AiTaskLifecycle::CheckExecutionBoundary(const AgentHandle activeAgent) {
        using enum AiTaskResumeDisposition;
        if (state_ == AiTaskState::Idle)
            return Failure<AiTaskResumeDisposition>(AIErrors::TaskTransitionInvalid);
        if (IsTerminal(state_))
            return Result<AiTaskResumeDisposition>::Success(AlreadyTerminal);
        if (!activeAgent.IsValid() || activeAgent != context_->agent || agentGenerationRetired_) {
            PublishCancelled(AiTaskCancellationReason::AgentGenerationRetired);
            return Result<AiTaskResumeDisposition>::Success(BecameTerminal);
        }
        if (context_->IsCancellationRequested()) {
            PublishCancelled(AiTaskCancellationReason::ContextCancelled);
            return Result<AiTaskResumeDisposition>::Success(BecameTerminal);
        }
        return Result<AiTaskResumeDisposition>::Success(Ready);
    }

    void AiTaskLifecycle::PublishCancelled(const AiTaskCancellationReason reason) noexcept {
        state_ = AiTaskState::Cancelled;
        terminalResult_.emplace(AiTaskTerminalResult{.state = AiTaskState::Cancelled, .cancellationReason = reason});
    }
}  // namespace Horo::AI
