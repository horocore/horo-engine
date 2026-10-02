#include "Horo/Runtime/Save/SaveAutosaveScheduler.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <utility>

namespace Horo::Runtime {
    namespace {
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
            // Teardown cannot propagate host callbacks or error-allocation failures.
            // Hosts use explicit BeginShutdown while dependencies live to observe cleanup errors.
        }
    }

    /** @copydoc SaveAutosaveScheduler::ObserveTerminal */
    Result<void> SaveAutosaveScheduler::ObserveTerminal() {
        if (!operation_.IsValid() || awaitingCapture_ || arbiter_->ActiveOperation() == operation_.Id())
            return Result<void>::Success();
        const auto terminal = operation_.Snapshot();
        if (!terminal || !terminal->IsTerminal())
            return Result<void>::Failure(MakeError(SaveErrors::CompletionInvalid));
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
    Result<void> SaveAutosaveScheduler::Admit(SaveOperationDescriptor operation, SaveArbiterAddress address) {
        if (operation.kind != SaveOperationKind::Save)
            return Result<void>::Failure(MakeError(SaveErrors::OperationInvalid));
        auto admitted = arbiter_->Admit({.operation = std::move(operation),
                                         .mode = SavePolicyMode::Auto,
                                         .address = std::move(address),
                                         .priority = SaveArbiterPriority::Background,
                                         .conflict = SaveArbiterConflictPolicy::Reject});
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
        if (const auto requested = [this]() {
            try {
                return barrier_->Request(operation_.Id(), last_.generation);
            } catch (...) {  // Injected host clock containment boundary.
                return Result<void>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed));
            }
        }(); requested.HasError()) {
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
        auto polled = [this, phase, &provenance, &participants, &limits]() {
            try {
                return barrier_->CaptureAtSafePoint(phase, last_.generation, provenance, std::move(participants), limits);
            } catch (...) {  // Injected host clock containment boundary; retain request ownership.
                snapshot_.blocked = true;
                snapshot_.pending = true;
                snapshot_.disposition = SaveAutosaveDisposition::Failed;
                return Result<SaveCaptureBarrierOutcome>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed));
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
        const RuntimeSaveCaptureLimits &limits) {
        using Return = Result<std::optional<SaveAutosaveCapture>>;
        if (const auto valid = ValidateMutation(); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
            return Return::Failure(MakeError(SaveErrors::SafePointInvalid));
        if (generation != last_.generation)
            return Return::Failure(MakeError(SaveErrors::GenerationStale));
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
        if (const auto admitted = Admit(std::move(operation), std::move(address)); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        return awaitingCapture_ ? Capture(phase, provenance, std::move(participants), limits) : Return::Success({});
    }

    /** @copydoc SaveAutosaveScheduler::CancelOwned */
    Result<void> SaveAutosaveScheduler::CancelOwned() {
        if (!operation_.IsValid())
            return Result<void>::Success();
        const auto cancelled = arbiter_->Cancel(operation_.Id());
        if (awaitingCapture_) {
            if (const auto result = CancelBarrier(cancelled); result.HasError())
                return result;
        }
        operation_ = {};
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::CancelBarrier */
    Result<void> SaveAutosaveScheduler::CancelBarrier(const SaveCancellationRequestResult cancelled) {
        const auto barrier = barrier_->Snapshot();
        if (barrier.HasError())
            return Result<void>::Failure(barrier.ErrorValue());
        if (barrier.Value().state == SaveBarrierState::Pending) {
            if (const auto result = [this]() {
                try {
                    return barrier_->Cancel(operation_.Id());
                } catch (...) {  // Injected host clock containment boundary; cleanup remains observable.
                    return Result<void>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed));
                }
            }(); result.HasError())
                return result;
        }
        if (const auto result = barrier_->Acknowledge(operation_.Id()); result.HasError())
            return result;
        if (cancelled == SaveCancellationRequestResult::Requested || cancelled == SaveCancellationRequestResult::AlreadyRequested) {
            if (const auto result = arbiter_->ObserveCancellation(operation_.Id()); result.HasError())
                return result;
        }
        awaitingCapture_ = false;
        return Result<void>::Success();
    }

    /** @copydoc SaveAutosaveScheduler::Cancel */
    Result<void> SaveAutosaveScheduler::Cancel() {
        if (const auto valid = ValidateMutation(); valid.HasError())
            return valid;
        const ExecutionScope scope(executing_);
        if (const auto cancelled = CancelOwned(); cancelled.HasError())
            return cancelled;
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
