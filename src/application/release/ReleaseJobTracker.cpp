#include "Horo/Release/ReleaseJobTracker.h"

#include "Horo/Foundation/Assertions.h"
#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Converts the closed stage enum to its fixed snapshot position. */
        [[nodiscard]] constexpr std::size_t StageIndex(const ReleaseStage stage) noexcept {
            return static_cast<std::size_t>(stage);
        }

        /** @brief Returns one stable failure without mutating the job. */
        [[nodiscard]] Error IllegalTransition() {
            return MakeError(ReleaseErrors::PipelineTransitionInvalid);
        }
    }  // namespace

    /** @copydoc ReleaseStagePlan::Includes */
    bool ReleaseStagePlan::Includes(const ReleaseStage stage) const noexcept {
        if (stage == ReleaseStage::Signing)
            return signing;
        if (stage == ReleaseStage::Publishing)
            return publishing;
        return StageIndex(stage) < ReleaseStageCount;
    }

    /** @copydoc ReleaseJobTracker::ReleaseJobTracker */
    ReleaseJobTracker::ReleaseJobTracker(const ReleaseJobId id, const ReleaseTargetId target, const OperationId operation,
                                         const ReleaseStagePlan plan)
        : plan_(plan) {
        HORO_INVARIANT_MSG(id.value != 0 && target.value != 0 && operation != 0, "Release job identities must be nonzero.");
        snapshot_.id = id;
        snapshot_.target = target;
        snapshot_.operation = operation;
        if (!plan.signing)
            snapshot_.stages[StageIndex(ReleaseStage::Signing)].state = ReleaseStageState::NotApplicable;
        if (!plan.publishing)
            snapshot_.stages[StageIndex(ReleaseStage::Publishing)].state = ReleaseStageState::NotApplicable;
    }

    /** @copydoc ReleaseJobTracker::Snapshot */
    ReleaseJobSnapshot ReleaseJobTracker::Snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    /** @copydoc ReleaseJobTracker::Start */
    Result<void> ReleaseJobTracker::Start() {
        std::lock_guard lock(mutex_);
        if (snapshot_.state != ReleaseJobState::Queued)
            return Result<void>::Failure(IllegalTransition());
        snapshot_.state = ReleaseJobState::Running;
        ++snapshot_.revision;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::BeginStage */
    Result<ReleaseStageAttemptId> ReleaseJobTracker::BeginStage(const ReleaseStage stage) {
        std::lock_guard lock(mutex_);
        const std::size_t index = StageIndex(stage);
        if (snapshot_.state != ReleaseJobState::Running || index >= ReleaseStageCount || !plan_.Includes(stage) || snapshot_.activeStage ||
            snapshot_.stages[index].state != ReleaseStageState::NotStarted || nextAttempt_ == 0)
            return Result<ReleaseStageAttemptId>::Failure(IllegalTransition());
        for (std::size_t predecessor = 0; predecessor < index; ++predecessor) {
            const ReleaseStageState state = snapshot_.stages[predecessor].state;
            if (state != ReleaseStageState::Succeeded && state != ReleaseStageState::NotApplicable)
                return Result<ReleaseStageAttemptId>::Failure(IllegalTransition());
        }
        if (stage == ReleaseStage::Publishing &&
            (!snapshot_.candidate || snapshot_.candidate->state != ReleaseCandidateState::FinalVerified))
            return Result<ReleaseStageAttemptId>::Failure(IllegalTransition());
        const ReleaseStageAttemptId attempt{nextAttempt_++};
        snapshot_.stages[index] = {ReleaseStageState::Running, attempt};
        snapshot_.activeStage = stage;
        snapshot_.progress.reset();
        ++snapshot_.revision;
        return Result<ReleaseStageAttemptId>::Success(attempt);
    }

    /** @copydoc ReleaseJobTracker::IsActiveAttempt */
    bool ReleaseJobTracker::IsActiveAttempt(const ReleaseStage stage, const ReleaseStageAttemptId attempt) const noexcept {
        const std::size_t index = StageIndex(stage);
        return index < ReleaseStageCount && snapshot_.activeStage == stage && snapshot_.stages[index].state == ReleaseStageState::Running &&
               snapshot_.stages[index].attempt == attempt;
    }

    /** @copydoc ReleaseJobTracker::CompleteActiveStage */
    void ReleaseJobTracker::CompleteActiveStage(const ReleaseStage stage) {
        snapshot_.stages[StageIndex(stage)].state = ReleaseStageState::Succeeded;
        snapshot_.activeStage.reset();
        ++snapshot_.revision;
    }

    /** @copydoc ReleaseJobTracker::CompleteStage */
    Result<void> ReleaseJobTracker::CompleteStage(const ReleaseStage stage, const ReleaseStageAttemptId attempt) {
        std::lock_guard lock(mutex_);
        if ((snapshot_.state != ReleaseJobState::Running && snapshot_.state != ReleaseJobState::Cancelling) ||
            stage == ReleaseStage::FinalizingMetadata || stage == ReleaseStage::FinalVerifying || !IsActiveAttempt(stage, attempt))
            return Result<void>::Failure(IllegalTransition());
        CompleteActiveStage(stage);
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::CompleteFinalization */
    Result<void> ReleaseJobTracker::CompleteFinalization(const ReleaseStageAttemptId attempt, const ReleaseCandidateId candidate) {
        std::lock_guard lock(mutex_);
        if (snapshot_.state != ReleaseJobState::Running || candidate.value == 0 || snapshot_.candidate ||
            !IsActiveAttempt(ReleaseStage::FinalizingMetadata, attempt))
            return Result<void>::Failure(IllegalTransition());
        snapshot_.candidate = ReleaseCandidateSnapshot{candidate, ReleaseCandidateState::Finalized};
        CompleteActiveStage(ReleaseStage::FinalizingMetadata);
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::CompleteFinalVerification */
    Result<void> ReleaseJobTracker::CompleteFinalVerification(const ReleaseStageAttemptId attempt) {
        std::lock_guard lock(mutex_);
        if (snapshot_.state != ReleaseJobState::Running || !snapshot_.candidate ||
            snapshot_.candidate->state != ReleaseCandidateState::Finalized || !IsActiveAttempt(ReleaseStage::FinalVerifying, attempt))
            return Result<void>::Failure(IllegalTransition());
        snapshot_.candidate->state = ReleaseCandidateState::FinalVerified;
        CompleteActiveStage(ReleaseStage::FinalVerifying);
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::FailStage */
    Result<void> ReleaseJobTracker::FailStage(const ReleaseStage stage, const ReleaseStageAttemptId attempt, Error cause) {
        using enum ReleaseJobState;
        std::lock_guard lock(mutex_);
        if ((snapshot_.state != Running && snapshot_.state != Cancelling) || !IsActiveAttempt(stage, attempt))
            return Result<void>::Failure(IllegalTransition());
        snapshot_.stages[StageIndex(stage)].state = ReleaseStageState::Failed;
        snapshot_.activeStage.reset();
        snapshot_.state = Failed;
        snapshot_.terminal = ReleaseFailed{snapshot_.target, stage, attempt, std::move(cause)};
        ++snapshot_.revision;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::RequestCancel */
    Result<void> ReleaseJobTracker::RequestCancel() {
        using enum ReleaseJobState;
        std::lock_guard lock(mutex_);
        if (snapshot_.state == Cancelling || snapshot_.state == Cancelled)
            return Result<void>::Success();
        if (snapshot_.state == Queued) {
            snapshot_.state = Cancelled;
            snapshot_.terminal = ReleaseCancelled{snapshot_.target, std::nullopt, std::nullopt};
        } else if (snapshot_.state == Running) {
            snapshot_.state = Cancelling;
        } else {
            return Result<void>::Failure(IllegalTransition());
        }
        ++snapshot_.revision;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::AcknowledgeCancellation */
    Result<void> ReleaseJobTracker::AcknowledgeCancellation() {
        std::lock_guard lock(mutex_);
        if (snapshot_.state != ReleaseJobState::Cancelling)
            return Result<void>::Failure(IllegalTransition());
        std::optional<ReleaseStageAttemptId> attempt;
        if (snapshot_.activeStage) {
            ReleaseStageSnapshot &stage = snapshot_.stages[StageIndex(*snapshot_.activeStage)];
            stage.state = ReleaseStageState::Cancelled;
            attempt = stage.attempt;
        }
        snapshot_.terminal = ReleaseCancelled{snapshot_.target, snapshot_.activeStage, attempt};
        snapshot_.activeStage.reset();
        snapshot_.state = ReleaseJobState::Cancelled;
        ++snapshot_.revision;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::FailAdmission */
    Result<void> ReleaseJobTracker::FailAdmission(Error cause) {
        std::lock_guard lock(mutex_);
        if (snapshot_.state != ReleaseJobState::Queued)
            return Result<void>::Failure(IllegalTransition());
        snapshot_.state = ReleaseJobState::Failed;
        snapshot_.terminal = ReleaseFailed{snapshot_.target, std::nullopt, std::nullopt, std::move(cause)};
        ++snapshot_.revision;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::FailJob */
    Result<void> ReleaseJobTracker::FailJob(Error cause) {
        using enum ReleaseJobState;
        std::lock_guard lock(mutex_);
        if ((snapshot_.state != Running && snapshot_.state != Cancelling) || snapshot_.activeStage)
            return Result<void>::Failure(IllegalTransition());
        snapshot_.state = Failed;
        snapshot_.terminal = ReleaseFailed{snapshot_.target, std::nullopt, std::nullopt, std::move(cause)};
        ++snapshot_.revision;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::UpdateProgress */
    Result<void> ReleaseJobTracker::UpdateProgress(const ReleaseStage stage, const ReleaseStageAttemptId attempt,
                                                   const std::uint64_t completed, const std::uint64_t total) {
        std::lock_guard lock(mutex_);
        if ((snapshot_.state != ReleaseJobState::Running && snapshot_.state != ReleaseJobState::Cancelling) ||
            !IsActiveAttempt(stage, attempt) || total == 0 || completed > total ||
            (snapshot_.progress && (snapshot_.progress->total != total || completed < snapshot_.progress->completed)))
            return Result<void>::Failure(IllegalTransition());
        snapshot_.progress = ReleaseStageProgress{stage, attempt, completed, total};
        ++snapshot_.revision;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseJobTracker::AppendDiagnostic */
    Result<ReleaseDiagnosticId> ReleaseJobTracker::AppendDiagnostic(const ReleaseStage stage, const ReleaseStageAttemptId attempt,
                                                                    ErrorCode code, const ErrorSeverity severity, std::string message) {
        using enum ReleaseJobState;
        std::lock_guard lock(mutex_);
        if ((snapshot_.state != Running && snapshot_.state != Cancelling) || !IsActiveAttempt(stage, attempt) || code.Value().empty() ||
            message.empty() || message.size() > MaximumReleaseDiagnosticMessageBytes || nextDiagnostic_ == 0)
            return Result<ReleaseDiagnosticId>::Failure(IllegalTransition());
        const ReleaseDiagnosticId id{nextDiagnostic_++};
        diagnostics_.emplace_back(id, snapshot_.id, snapshot_.target, snapshot_.operation, stage, attempt, std::move(code), severity,
                                  std::move(message));
        snapshot_.recentDiagnostics.push_back(id);
        if (diagnostics_.size() > MaximumReleaseDiagnostics) {
            diagnostics_.pop_front();
            snapshot_.recentDiagnostics.erase(snapshot_.recentDiagnostics.begin());
        }
        ++snapshot_.revision;
        return Result<ReleaseDiagnosticId>::Success(id);
    }

    /** @copydoc ReleaseJobTracker::Diagnostic */
    std::optional<ReleaseDiagnostic> ReleaseJobTracker::Diagnostic(const ReleaseDiagnosticId id) const {
        std::lock_guard lock(mutex_);
        const auto found = std::ranges::find_if(diagnostics_, [id](const ReleaseDiagnostic &diagnostic) {
            return diagnostic.id == id;
        });
        if (found == diagnostics_.end())
            return std::nullopt;
        return *found;
    }

    /** @copydoc ReleaseJobTracker::FinishSuccess */
    Result<void> ReleaseJobTracker::FinishSuccess() {
        std::lock_guard lock(mutex_);
        if (snapshot_.state != ReleaseJobState::Running || snapshot_.activeStage || !snapshot_.candidate ||
            snapshot_.candidate->state != ReleaseCandidateState::FinalVerified)
            return Result<void>::Failure(IllegalTransition());
        for (const ReleaseStageSnapshot &stage : snapshot_.stages) {
            if (stage.state != ReleaseStageState::Succeeded && stage.state != ReleaseStageState::NotApplicable)
                return Result<void>::Failure(IllegalTransition());
        }
        snapshot_.state = ReleaseJobState::Succeeded;
        snapshot_.terminal = ReleaseSucceeded{snapshot_.target, snapshot_.candidate->id};
        ++snapshot_.revision;
        return Result<void>::Success();
    }
}  // namespace Horo::Release
