#include "Horo/Runtime/Save/RuntimeSaveSession.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "RuntimeSaveSessionInternal.h"

#include <new>

namespace Horo::Runtime {
    namespace {
        class PreparationScope final {
        public:
            explicit PreparationScope(bool &active) noexcept : active_(active) {
                active_ = true;
            }

            PreparationScope(const PreparationScope &) = delete;
            PreparationScope &operator=(const PreparationScope &) = delete;

            ~PreparationScope() {
                active_ = false;
            }

        private:
            bool &active_;
        };

        Result<bool> PreparationFailure(RuntimeSaveSessionDetail::Work &work, Error error) {
            (void)RuntimeSaveSessionDetail::FailOwned(work, error, SaveOperationCommitOutcome::NotCommitted);
            return Result<bool>::Failure(std::move(error));
        }
    }  // namespace

    /** @copydoc RuntimeSaveSession::SettleCapturePreparationRetirement */
    bool RuntimeSaveSession::SettleCapturePreparationRetirement(const std::shared_ptr<RuntimeSaveSessionDetail::Work> &work) {
        if (!retireAfterCapturePreparation_)
            return false;
        retireAfterCapturePreparation_ = false;
        (void)RetireInternal();
        (void)work->producer.ObserveCancellation();
        return true;
    }

    /** @copydoc RuntimeSaveSession::RejectCapturePreparation */
    Result<bool> RuntimeSaveSession::RejectCapturePreparation(const std::shared_ptr<RuntimeSaveSessionDetail::Work> &work, Error error) {
        (void)SettleCapturePreparationRetirement(work);
        return PreparationFailure(*work, std::move(error));
    }

    /** @copydoc RuntimeSaveSession::ValidateCapturePreparation */
    Result<bool> RuntimeSaveSession::ValidateCapturePreparation(const std::shared_ptr<RuntimeSaveSessionDetail::Work> &work,
                                                                const RuntimeSaveCapturePreparationEvidence &observed,
                                                                const RuntimeSaveCaptureProvenance &provenance) {
        if (SettleCapturePreparationRetirement(work))
            return Result<bool>::Success(false);
        if (closed_ || work_ != work)
            return PreparationFailure(*work, MakeError(SaveErrors::LifecycleUnavailable));
        if (work->producer.ObserveCancellation() == SaveCancellationObservation::Cancelled) {
            ReleaseBarrier();
            return Result<bool>::Success(false);
        }
        if (observed.generation != generation_ || observed.provenance != provenance)
            return PreparationFailure(*work, MakeError(SaveErrors::GenerationStale));
        if (work->handle.Snapshot()->IsTerminal())
            return Result<bool>::Success(false);
        if (!observed.ready)
            return Result<bool>::Success(false);
        capturePreparedFor_ = provenance;
        return Result<bool>::Success(true);
    }

    /** @copydoc RuntimeSaveSession::PrepareCaptureOwner */
    Result<bool> RuntimeSaveSession::PrepareCaptureOwner(const std::shared_ptr<RuntimeSaveSessionDetail::Work> &work,
                                                         const RuntimeSaveCaptureProvenance &provenance) {
        if (!capturePreparation_)
            return Result<bool>::Success(true);
        if (capturePreparedFor_ && *capturePreparedFor_ == provenance)
            return Result<bool>::Success(true);
        const auto preparation = capturePreparation_;  // Owner pin only; no live preparation object enters worker Work.
        try {
            Result<RuntimeSaveCapturePreparationEvidence> prepared = [&] {
                const PreparationScope guard{preparingCapture_};
                return preparation->Prepare({work->handle, {generation_, provenance}, work->cancellation.Token()});
            }();
            if (prepared.HasError())
                return RejectCapturePreparation(work, prepared.ErrorValue());
            return ValidateCapturePreparation(work, prepared.Value(), provenance);
        } catch (const std::bad_alloc &) {
            return RejectCapturePreparation(work, MakeError(SaveErrors::OperationAllocationFailed));
        } catch (...) {
            return RejectCapturePreparation(work, MakeError(SaveErrors::LifecycleCallbackFailed));
        }
    }
}  // namespace Horo::Runtime
