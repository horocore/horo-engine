#include "Horo/Runtime/Save/SaveErrors.h"
#include "RuntimeSaveSessionInternal.h"

#include <new>

namespace Horo::Runtime::RuntimeSaveSessionDetail {
    /** @copydoc OwnedProducer */
    SaveOperationController *OwnedProducer(Work &work) noexcept {
        if (work.preparedRestore && work.preparedRestore->operation.Handle().IsValid())
            return &work.preparedRestore->operation;
        return work.producer.Handle().IsValid() ? &work.producer : nullptr;
    }

    /** @copydoc CancelRestore */
    void CancelRestore(Work &work) noexcept {
        if (auto *producer = OwnedProducer(work)) {
            (void)producer->RequestShutdownCancellation();
            (void)producer->ObserveCancellation();
        } else {
            (void)work.handle.RequestCancellation();
        }
        if (work.handoffAttempted)
            work.handoff->Cancel();
    }

    /** @copydoc FailOwned */
    Result<void> FailOwned(Work &work, Error error, const SaveOperationCommitOutcome outcome) {
        if (auto *producer = OwnedProducer(work)) {
            (void)producer->Fail(error, outcome);
        } else {
            // A receiver may already own a queued Scene: request retirement, never guess its publication outcome.
            (void)work.handle.RequestCancellation();
        }
        if (work.handoffAttempted)
            work.handoff->Cancel();
        return Result<void>::Failure(std::move(error));
    }

    /** @copydoc PollRestoreCancellation */
    void PollRestoreCancellation(Work &work) noexcept {
        if (work.handle.Snapshot()->cancellationRequested)
            work.handoff->Cancel();
    }

    /** @copydoc RestoreSettled */
    bool RestoreSettled(const Work &work) noexcept {
        return !work.handoffAttempted || work.handoff->IsSettled();
    }

    namespace {
        /** @brief Contains arbitrary owner-adapter faults while retaining the actual packet for ownership inspection. */
        Result<void> Invoke(Work &work) {
            auto allocationFailure = MakeError(SaveErrors::OperationAllocationFailed);
            auto callbackFailure = MakeError(SaveErrors::LifecycleCallbackFailed);
            try {
                return work.handoff->Accept(*work.preparedRestore);
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(std::move(allocationFailure));
            } catch (...) {
                return Result<void>::Failure(std::move(callbackFailure));
            }
        }

        /** @brief Success is not ownership transfer unless the original sole controller has actually moved. */
        Result<void> ValidateDisposition(Work &work, Result<void> result) {
            if (result.HasValue() && OwnedProducer(work))
                return FailOwned(work, MakeError(SaveErrors::RestoreAdapterContractInvalid), SaveOperationCommitOutcome::NotCommitted);
            if (result.HasValue())
                return result;
            work.handoffFailure.emplace(std::move(result).ErrorValue());
            return FailOwned(work, *work.handoffFailure, SaveOperationCommitOutcome::NotCommitted);
        }
    }  // namespace

    /** @copydoc DeliverRestore */
    Result<void> DeliverRestore(Work &work, RuntimeSaveRestoreStaging staging) {
        if (work.handoffAttempted || !work.handoff || !work.producer.Handle().IsValid())
            return FailOwned(work, MakeError(SaveErrors::RestoreTransitionInvalid), SaveOperationCommitOutcome::NotCommitted);
        if (work.producer.ObserveCancellation() == SaveCancellationObservation::Cancelled)
            return Result<void>::Success();
        work.preparedRestore.emplace(
            RuntimeSavePreparedRestore{work.context, work.participants, std::move(work.producer), std::move(staging), work.source});
        work.handoffAttempted = true;
        return ValidateDisposition(work, Invoke(work));
    }
}  // namespace Horo::Runtime::RuntimeSaveSessionDetail
