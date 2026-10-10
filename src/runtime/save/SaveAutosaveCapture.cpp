#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Runtime/Save/SaveAutosaveScheduler.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Keeps optional retry evidence bound to this exact autosave capture incarnation. */
        [[nodiscard]] bool IsCaptureOperationValid(const SaveOperationDescriptor &operation,
                                                   const std::optional<SaveArbiterRetryDescriptor> &retry,
                                                   const SaveRuntimeGeneration generation) noexcept {
            return operation.kind == SaveOperationKind::Save && (!retry || retry->preconditions.runtime == generation);
        }

        /** @brief Rejects nested mutations through adapters and operation terminal observers. */
        class ExecutionScope final {
        public:
            explicit ExecutionScope(bool &executing) noexcept : executing_(executing) {
                static_cast<void>(std::exchange(executing, true));
            }

            ~ExecutionScope() {
                executing_ = false;
            }

            ExecutionScope(const ExecutionScope &) = delete;
            ExecutionScope &operator=(const ExecutionScope &) = delete;

        private:
            bool &executing_;
        };
    }  // namespace

    /** @copydoc SaveAutosaveScheduler::~SaveAutosaveScheduler */
    SaveAutosaveScheduler::~SaveAutosaveScheduler() {
        try {
            static_cast<void>(BeginShutdown());
        } catch (...) {
            Log::Logger::WriteEmergency("runtime.save.autosave", Log::Level::Error,
                                        "Autosave teardown failed unexpectedly; ownership cleanup could not be confirmed.");
        }
    }

    /** @copydoc SaveAutosaveScheduler::ObserveTerminal */
    Result<void> SaveAutosaveScheduler::ObserveTerminal() {
        if (!operation_.IsValid() || awaitingCapture_ || arbiter_->ActiveOperation() == operation_.Id())
            return Result<void>::Success();
        const auto terminal = operation_.Snapshot();
        if (!terminal)
            return Result<void>::Failure(MakeError(SaveErrors::CompletionInvalid));
        if (!terminal->IsTerminal())
            return Result<void>::Success();
        operation_ = {};
        if (terminal->state == SaveOperationState::Failed) {
            snapshot_.disposition = SaveAutosaveDisposition::Failed;
            snapshot_.pending = true;
            snapshot_.blocked = true;
            return Result<void>::Failure(*terminal->terminalError);
        }
        snapshot_.disposition =
            terminal->state == SaveOperationState::Cancelled ? SaveAutosaveDisposition::Cancelled : SaveAutosaveDisposition::Completed;
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::Admit */
    Result<void> SaveAutosaveScheduler::Admit(SaveOperationDescriptor operation, SaveArbiterAddress address,
                                              std::optional<SaveArbiterRetryDescriptor> retry) {
        if (!IsCaptureOperationValid(operation, retry, last_.generation))
            return Result<void>::Failure(MakeError(SaveErrors::OperationInvalid));
        auto callbackFailure = Result<void>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed));
        auto admitted = arbiter_->Admit({.operation = std::move(operation),
                                         .mode = SavePolicyMode::Auto,
                                         .address = std::move(address),
                                         .priority = SaveArbiterPriority::Background,
                                         .conflict = SaveArbiterConflictPolicy::Reject,
                                         .retry = std::move(retry)});
        if (admitted.HasError()) {
            snapshot_.cooldownRemaining = policy_.cooldown;
            return Result<void>::Failure(admitted.ErrorValue());
        }
        operation_ = admitted.Value().handle;
        snapshot_.operation = operation_.Id();
        snapshot_.pending = false;
        snapshot_.pendingAge = {};
        snapshot_.cooldownRemaining = policy_.cooldown;
        if (const auto selected = arbiter_->StartNext(); !selected || selected->operation.operation != operation_.Id())
            return Result<void>::Failure(MakeError(SaveErrors::ArbiterInvalid));
        if (auto advanced = arbiter_->Advance(operation_.Id(), SaveArbiterState::WaitingForSafePoint); advanced.HasError())
            return advanced;
        if (arbiter_->ActiveOperation() != operation_.Id())
            return ObserveTerminal();
        static_assert(std::is_nothrow_move_constructible_v<Result<void>>);
        // Host failures transfer a prepared result; exception translation cannot itself allocate.
        const auto requested = [this, failure = std::move(callbackFailure)]() mutable noexcept {
            try {
                return barrier_->Request(operation_.Id(), last_.generation);
            } catch (...) {  // Injected host clock containment boundary.
                return std::move(failure);
            }
        }();
        if (requested.HasError()) {
            static_cast<void>(arbiter_->Fail(operation_.Id(), requested.ErrorValue()));
            return ObserveTerminal();
        }
        awaitingCapture_ = true;
        snapshot_.disposition = SaveAutosaveDisposition::Capturing;
        return arbiter_->Advance(operation_.Id(), SaveArbiterState::Capturing);
    }

    /** @copydoc SaveAutosaveScheduler::RetireCapture */
    Result<void> SaveAutosaveScheduler::RetireCapture() {
        const auto terminal = operation_.Snapshot();
        if (const auto cancelled = CancelOwned(); cancelled.HasError())
            return cancelled;
        if (terminal && terminal->state == SaveOperationState::Failed) {
            snapshot_.pending = true;
            snapshot_.blocked = true;
            snapshot_.disposition = SaveAutosaveDisposition::Failed;
            return Result<void>::Failure(*terminal->terminalError);
        }
        snapshot_.disposition = SaveAutosaveDisposition::Cancelled;
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::Capture */
    Result<std::optional<SaveAutosaveCapture>> SaveAutosaveScheduler::Capture(const RuntimePhase phase,
                                                                              const RuntimeSaveCaptureProvenance &provenance,
                                                                              SaveParticipantRegistrySnapshot participants,
                                                                              const RuntimeSaveCaptureLimits &limits) {
        using Return = Result<std::optional<SaveAutosaveCapture>>;
        if (arbiter_->ActiveOperation() != operation_.Id()) {
            if (const auto retired = RetireCapture(); retired.HasError())
                return Return::Failure(retired.ErrorValue());
            return Return::Success({});
        }
        // Observe parent/deadline/caller cancellation before invoking any capture adapter.
        const auto cancellation = arbiter_->PollCancellation(operation_.Id());
        if (cancellation.HasError())
            return Return::Failure(cancellation.ErrorValue());
        if (cancellation.Value()) {
            if (const auto retired = RetireCapture(); retired.HasError())
                return Return::Failure(retired.ErrorValue());
            return Return::Success({});
        }
        static_assert(std::is_nothrow_move_constructible_v<Result<SaveCaptureBarrierOutcome>>);
        auto polled = [this, phase, &provenance, &participants, &limits,
                       failure =
                           Result<SaveCaptureBarrierOutcome>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed))]() mutable noexcept {
            try {
                return barrier_->CaptureAtSafePoint(phase, last_.generation, provenance, std::move(participants), limits);
            } catch (...) {  // Injected host clock containment boundary; retain request ownership.
                snapshot_.blocked = true;
                snapshot_.pending = true;
                snapshot_.disposition = SaveAutosaveDisposition::Failed;
                return std::move(failure);
            }
        }();
        if (polled.HasError())
            return Return::Failure(polled.ErrorValue());
        return CompleteCapture(std::move(polled).Value());
    }

    /** @copydoc SaveAutosaveScheduler::CompleteCapture */
    Result<std::optional<SaveAutosaveCapture>> SaveAutosaveScheduler::CompleteCapture(SaveCaptureBarrierOutcome outcome) {
        using Return = Result<std::optional<SaveAutosaveCapture>>;
        snapshot_.lastBarrier = outcome.barrier;
        if (outcome.barrier.state == SaveBarrierState::Pending)
            return Return::Success({});
        if (const auto acknowledged = barrier_->Acknowledge(operation_.Id()); acknowledged.HasError())
            return Return::Failure(acknowledged.ErrorValue());
        awaitingCapture_ = false;
        if (!outcome.capture) {
            const Error error = outcome.error.value_or(MakeError(SaveErrors::LifecycleSuspended));
            if (const auto failed = arbiter_->Fail(operation_.Id(), error); failed.HasError())
                return Return::Failure(failed.ErrorValue());
            operation_ = {};
            snapshot_.pending = true;
            snapshot_.blocked = true;
            snapshot_.disposition = SaveAutosaveDisposition::Failed;
            return Return::Failure(error);
        }
        if (const auto advanced = arbiter_->Advance(operation_.Id(), SaveArbiterState::Encoding); advanced.HasError())
            return Return::Failure(advanced.ErrorValue());
        if (arbiter_->ActiveOperation() != operation_.Id()) {
            static_cast<void>(ObserveTerminal());
            return Return::Success({});
        }
        snapshot_.disposition = SaveAutosaveDisposition::Saving;
        return Return::Success(SaveAutosaveCapture{last_.generation, operation_, std::move(*outcome.capture)});
    }

    /** @copydoc SaveAutosaveScheduler::ReadyForAdmission */
    Result<bool> SaveAutosaveScheduler::ReadyForAdmission() const {
        if (operation_.IsValid() || !snapshot_.pending || snapshot_.cooldownRemaining > Duration{} ||
            arbiter_->ActiveOperation().has_value() || arbiter_->QueuedCount() != 0)
            return Result<bool>::Success(false);
        const auto barrier = barrier_->Snapshot();
        if (barrier.HasError())
            return Result<bool>::Failure(barrier.ErrorValue());
        return Result<bool>::Success(barrier.Value().state == SaveBarrierState::Idle);
    }

    /** @copydoc SaveAutosaveScheduler::CommitAtSafePoint */
    Result<std::optional<SaveAutosaveCapture>> SaveAutosaveScheduler::CommitAtSafePoint(
        const RuntimePhase phase, const SaveRuntimeGeneration generation, SaveOperationDescriptor operation, SaveArbiterAddress address,
        const RuntimeSaveCaptureProvenance &provenance, SaveParticipantRegistrySnapshot participants,
        const RuntimeSaveCaptureLimits &limits, std::optional<SaveArbiterRetryDescriptor> retry) {
        using Return = Result<std::optional<SaveAutosaveCapture>>;
        if (const auto valid = ValidateSafePoint(phase, generation); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        const ExecutionScope scope(executing_);
        if (const auto observed = ObserveTerminal(); observed.HasError())
            return Return::Failure(observed.ErrorValue());
        if (last_.activity != SaveAutosaveActivity::Active || snapshot_.blocked)
            return Return::Success({});
        if (awaitingCapture_)
            return Capture(phase, provenance, std::move(participants), limits);
        const auto ready = ReadyForAdmission();
        if (ready.HasError())
            return Return::Failure(ready.ErrorValue());
        if (!ready.Value())
            return Return::Success({});
        if (const auto admitted = Admit(std::move(operation), std::move(address), std::move(retry)); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        return awaitingCapture_ ? Capture(phase, provenance, std::move(participants), limits) : Return::Success({});
    }

    /** @copydoc SaveAutosaveScheduler::CancelOwned */
    Result<void> SaveAutosaveScheduler::CancelOwned() {
        if (!operation_.IsValid())
            return Result<void>::Success();
        const auto cancelled = arbiter_->Cancel(operation_.Id());
        auto result = awaitingCapture_ ? CancelBarrier(cancelled) : Result<void>::Success();
        if (!awaitingCapture_)
            operation_ = {};
        return result;
    }

    /** @copydoc SaveAutosaveScheduler::CancelBarrier */
    Result<void> SaveAutosaveScheduler::CancelBarrier(const SaveCancellationRequestResult cancelled) {
        const auto barrier = barrier_->Snapshot();
        if (barrier.HasError())
            return Result<void>::Failure(barrier.ErrorValue());
        auto timing = Result<void>::Success();
        if (barrier.Value().state == SaveBarrierState::Pending) {
            timing = barrier_->Cancel(operation_.Id());
            if (timing.HasError() && !RetainCancelledBarrier())
                return timing;
        }
        if (const auto result = barrier_->Acknowledge(operation_.Id()); result.HasError())
            return result;
        if (cancelled == SaveCancellationRequestResult::Requested || cancelled == SaveCancellationRequestResult::AlreadyRequested) {
            if (const auto result = arbiter_->ObserveCancellation(operation_.Id()); result.HasError())
                return result;
        }
        awaitingCapture_ = false;
        return timing;
    }

    /** @copydoc SaveAutosaveScheduler::RetainCancelledBarrier */
    bool SaveAutosaveScheduler::RetainCancelledBarrier() {
        const auto terminal = barrier_->Snapshot();
        if (terminal.HasError() || terminal.Value().operation != operation_.Id() || terminal.Value().state != SaveBarrierState::Cancelled)
            return false;
        snapshot_.lastBarrier = terminal.Value();
        return true;
    }

    /** @copydoc SaveAutosaveScheduler::Cancel */
    Result<void> SaveAutosaveScheduler::Cancel() {
        if (const auto valid = ValidateMutation(); valid.HasError())
            return valid;
        const ExecutionScope scope(executing_);
        if (const auto cancelled = CancelOwned(); cancelled.HasError()) {
            snapshot_.blocked = true;
            snapshot_.disposition = SaveAutosaveDisposition::Failed;
            return cancelled;
        }
        snapshot_.pending = false;
        snapshot_.pendingAge = {};
        snapshot_.blocked = false;
        snapshot_.disposition = SaveAutosaveDisposition::Cancelled;
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::Resume */
    Result<void> SaveAutosaveScheduler::Resume() {
        if (const auto valid = ValidateMutation(); valid.HasError())
            return valid;
        snapshot_.blocked = false;
        snapshot_.disposition = snapshot_.pending ? SaveAutosaveDisposition::Pending : SaveAutosaveDisposition::Armed;
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::ReplaceSession */
    Result<void> SaveAutosaveScheduler::ReplaceSession(const SaveAutosaveClockSample &initial) {
        if (const auto valid = ValidateMutation(); valid.HasError())
            return valid;
        if (!initial.generation.IsValid() || initial.generation == last_.generation || initial.gameplay < Duration{} ||
            initial.monotonic < Duration{} || initial.activity < SaveAutosaveActivity::Active ||
            initial.activity > SaveAutosaveActivity::Inactive)
            return Result<void>::Failure(MakeError(SaveErrors::PolicyInvalid));
        const ExecutionScope scope(executing_);
        if (const auto cancelled = CancelOwned(); cancelled.HasError())
            return cancelled;
        Reset(initial);
        snapshot_.disposition = SaveAutosaveDisposition::Cancelled;
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::BeginShutdown */
    Result<void> SaveAutosaveScheduler::BeginShutdown() {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return owner;
        if (closed_)
            return Result<void>::Success();
        if (const auto cancelled = Cancel(); cancelled.HasError())
            return cancelled;
        closed_ = true;
        snapshot_.disposition = SaveAutosaveDisposition::Closed;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
