#include "Horo/Runtime/Save/RuntimeSaveSession.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <mutex>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace RuntimeSaveSessionDetail {
        /** @brief Worker closures and consumer handles pin this state; no session/host borrow crosses into a worker.
         * The mutex protects only detached staging handoff. Participant/user callbacks never execute under it.
         * Producer ownership stays fixed for Save, and transfers to the restore transaction only after the Load job settles.
         */
        struct Work final {
            explicit Work(SaveOperationController value, CancellationSource cancel)
                : producer(std::move(value)), handle(producer.Handle()), cancellation(std::move(cancel)) {}

            SaveOperationController producer;
            SaveOperationHandle handle;
            CancellationSource cancellation;
            SaveParticipantRegistrySnapshot participants;
            StagedRestoreContext context;
            std::shared_ptr<IRuntimeSaveWriter> writer;
            std::shared_ptr<IRuntimeSaveRestoreSource> source;
            RuntimeSaveSnapshot capture;
            std::mutex mutex;
            std::optional<RuntimeSaveRestoreStaging> staging;
            std::optional<JobHandle> job;
            bool completionPublished{};
        };
    }  // namespace RuntimeSaveSessionDetail

    namespace {
        using RuntimeSaveSessionDetail::Work;

        /** @brief Holds the existing owner transition fence across host callbacks and resets it on every exit. */
        class MutationScope final {
        public:
            explicit MutationScope(bool &active) noexcept : active_(active) {
                active_ = true;
            }

            ~MutationScope() {
                active_ = false;
            }

            MutationScope(const MutationScope &) = delete;
            MutationScope &operator=(const MutationScope &) = delete;

        private:
            bool &active_;
        };

        /** @brief Reads detached settlement without admitting a nested public owner operation. */
        bool Settled(const std::shared_ptr<Work> &work) {
            if (!work || !work->job)
                return true;
            const auto snapshot = work->job->Snapshot();
            return snapshot && snapshot->state != JobState::Queued && snapshot->state != JobState::Running;
        }

        /** @brief Reuses the existing typed lifecycle errors. */
        Result<void> Failure(const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        }

        /** @brief Publishes original failure unless cancellation won first; commit errors retain uncertainty. */
        Result<void> Fail(Work &work, Error error, SaveOperationCommitOutcome outcome = SaveOperationCommitOutcome::NotCommitted) {
            (void)work.producer.Fail(error, outcome);
            return Result<void>::Failure(std::move(error));
        }

        /** @brief Executes detached preparation/publication; cancellation linearizes at the existing commit gate. */
        Result<void> Write(Work &work, const CancellationToken &cancellation) {
            if (work.producer.PublishProgress(SaveOperationStage::Serializing, {}) != SaveOperationTransitionResult::Applied)
                return JobCancelled();
            auto prepared = work.writer->Prepare(work.capture, cancellation);
            if (prepared.HasError())
                return Fail(work, std::move(prepared).ErrorValue());
            if (cancellation.IsCancellationRequested()) {
                (void)work.handle.RequestCancellation();
                (void)work.producer.ObserveCancellation();
                return JobCancelled();
            }
            if (work.producer.PublishProgress(SaveOperationStage::WritingTemporary, {1, 1}) != SaveOperationTransitionResult::Applied)
                return JobCancelled();
            if (work.producer.BeginCommit() != SaveCommitGateResult::Entered)
                return JobCancelled();
            try {
                auto committed = work.writer->Commit();
                if (committed.HasError())
                    return Fail(work, std::move(committed).ErrorValue(), SaveOperationCommitOutcome::Unknown);
            } catch (...) {
                return Fail(work, MakeError(SaveErrors::LifecycleCallbackFailed), SaveOperationCommitOutcome::Unknown);
            }
            (void)work.producer.Complete(SaveOperationCommitOutcome::Committed);
            return Result<void>::Success();
        }

        /** @brief Stages only detached input; all runtime-sensitive preparation stays on the owner. */
        Result<void> Read(Work &work, const CancellationToken &cancellation) {
            if (work.producer.PublishProgress(SaveOperationStage::VerifyingArchive, {}) != SaveOperationTransitionResult::Applied)
                return JobCancelled();
            auto staged = work.source->Stage(work.context, work.participants, cancellation);
            if (staged.HasError())
                return Fail(work, std::move(staged).ErrorValue());
            if (work.producer.ObserveCancellation() == SaveCancellationObservation::Cancelled)
                return JobCancelled();
            std::lock_guard lock(work.mutex);
            work.staging.emplace(std::move(staged).Value());
            return Result<void>::Success();
        }

        /** @brief The safe-point coordinator invokes only this synchronous owner-local executor. */
        class Executor final : public ISaveSafePointExecutor {
        public:
            explicit Executor(Work &work) : work_(work) {}

            Result<void> Capture(OperationId, SaveRuntimeGeneration) override {
                return Result<void>::Success();
            }

            Result<void> Restore(OperationId, SaveRuntimeGeneration generation) override {
                std::optional<RuntimeSaveRestoreStaging> staging;
                {
                    std::lock_guard lock(work_.mutex);
                    staging = std::move(work_.staging);
                    work_.staging.reset();
                }
                if (!staging)
                    return Failure(SaveErrors::CompletionInvalid);
                auto created = StagedRestoreTransaction::Create(work_.context, std::move(work_.producer), work_.participants,
                                                                std::move(staging->participants));
                if (created.HasError())
                    return Result<void>::Failure(std::move(created).ErrorValue());
                auto transaction = std::move(created).Value();
                if (staging->references) {
                    auto bound = transaction.SetReferenceResolver(std::move(staging->references));
                    if (bound.HasError()) {
                        transaction.Rollback(bound.ErrorValue());
                        return bound;
                    }
                }
                auto prepared = transaction.Prepare();
                if (prepared.HasError())
                    return prepared;
                return transaction.Activate({generation.registry, generation.runtime, generation.scene});
            }

        private:
            Work &work_;
        };
    }  // namespace

    /** @copydoc RuntimeSaveRequest::RuntimeSaveRequest */
    RuntimeSaveRequest::RuntimeSaveRequest(std::shared_ptr<Work> work) noexcept : work_(std::move(work)) {}

    /** @copydoc RuntimeSaveRequest::Operation */
    SaveOperationHandle RuntimeSaveRequest::Operation() const noexcept {
        return work_->handle;
    }

    /** @copydoc RuntimeSaveRequest::RequestCancellation */
    SaveCancellationRequestResult RuntimeSaveRequest::RequestCancellation() const noexcept {
        const auto result = work_->handle.RequestCancellation();
        if (result == SaveCancellationRequestResult::Requested || result == SaveCancellationRequestResult::AlreadyRequested)
            work_->cancellation.RequestCancellation();
        return result;
    }

    /** @copydoc RuntimeSaveSession::RuntimeSaveSession */
    RuntimeSaveSession::RuntimeSaveSession(JobSystem &jobs, SaveCaptureBarrier &barrier, SaveParticipantRegistrySnapshot participants,
                                           SaveRuntimeGeneration generation, RuntimeSaveCaptureLimits limits, JobResource workerLane,
                                           std::shared_ptr<SaveSafePointCoordinator> safePoints) noexcept
        : jobs_(jobs), barrier_(barrier), participants_(std::move(participants)), generation_(generation), limits_(limits),
          workerLane_(workerLane), safePoints_(std::move(safePoints)), owner_(std::this_thread::get_id()) {}

    /** @copydoc RuntimeSaveSession::Create */
    Result<std::unique_ptr<RuntimeSaveSession>> RuntimeSaveSession::Create(JobSystem &jobs, SaveCaptureBarrier &barrier,
                                                                           SaveParticipantRegistrySnapshot participants,
                                                                           SaveRuntimeGeneration generation,
                                                                           RuntimeSaveCaptureLimits limits, JobResource workerLane) {
        using Return = Result<std::unique_ptr<RuntimeSaveSession>>;
        if (!participants.IsValid() || !generation.IsValid() || participants.Generation() != generation.registry ||
            limits.maximumParticipants == 0 || limits.maximumParticipants > MaximumSaveParticipantCount ||
            (workerLane != JobResource::Cpu && workerLane != JobResource::Io))
            return Return::Failure(MakeError(SaveErrors::LifecycleInvalid));
        try {
            auto coordinator = SaveSafePointCoordinator::Create(generation, 1);
            if (coordinator.HasError())
                return Return::Failure(std::move(coordinator).ErrorValue());
            return Return::Success(
                std::unique_ptr<RuntimeSaveSession>(new RuntimeSaveSession(jobs, barrier, std::move(participants), generation, limits,
                                                                           workerLane, std::move(coordinator).Value())));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        }
    }

    /** @copydoc RuntimeSaveSession::~RuntimeSaveSession */
    RuntimeSaveSession::~RuntimeSaveSession() {
        (void)Retire();
    }

    /** @copydoc RuntimeSaveSession::ValidateOwner */
    Result<void> RuntimeSaveSession::ValidateOwner() const {
        if (owner_ != std::this_thread::get_id())
            return Failure(SaveErrors::ThreadAffinityViolation);
        if (mutating_)
            return Failure(SaveErrors::LifecycleReentrant);
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSaveSession::Admit */
    Result<std::shared_ptr<Work>> RuntimeSaveSession::Admit(SaveOperationDescriptor operation) {
        using Return = Result<std::shared_ptr<Work>>;
        if (closed_)
            return Return::Failure(MakeError(SaveErrors::LifecycleUnavailable));
        if (work_)
            return Return::Failure(MakeError(SaveErrors::OperationInProgress));
        CancellationSource cancellation(operation.parentCancellation);
        operation.parentCancellation = cancellation.Token();
        auto producer = CreateSaveOperation(operation);
        if (producer.HasError())
            return Return::Failure(std::move(producer).ErrorValue());
        auto work = std::make_shared<Work>(std::move(producer).Value(), std::move(cancellation));
        work->participants = participants_;
        work->context = {operation.operation, generation_.registry, generation_.runtime, generation_.scene, limits_.maximumParticipants};
        auto admitted = safePoints_->Admit(
            {operation.operation, operation.kind == SaveOperationKind::Save ? SaveSafePointAction::Capture : SaveSafePointAction::Restore,
             generation_});
        if (admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        work_ = work;
        return Return::Success(std::move(work));
    }

    /** @copydoc RuntimeSaveSession::Save */
    Result<RuntimeSaveRequest> RuntimeSaveSession::Save(SaveOperationDescriptor operation, std::shared_ptr<IRuntimeSaveWriter> writer) {
        using Return = Result<RuntimeSaveRequest>;
        if (auto valid = ValidateOwner(); valid.HasError())
            return Return::Failure(std::move(valid).ErrorValue());
        const MutationScope mutation(mutating_);
        if (operation.kind != SaveOperationKind::Save || !writer)
            return Return::Failure(MakeError(SaveErrors::OperationInvalid));
        bool admittedHere = false;
        try {
            auto admitted = Admit(operation);
            if (admitted.HasError())
                return Return::Failure(std::move(admitted).ErrorValue());
            admittedHere = true;
            work_->writer = std::move(writer);
            auto requested = barrier_.Request(operation.operation, generation_);
            if (requested.HasError()) {
                (void)Fail(*work_, requested.ErrorValue());
                return Return::Success(RuntimeSaveRequest(work_));
            }
            barrierPending_ = true;
            return Return::Success(RuntimeSaveRequest(work_));
        } catch (const std::bad_alloc &) {
            if (!admittedHere)
                return Return::Failure(MakeError(SaveErrors::OperationAllocationFailed));
            (void)Fail(*work_, MakeError(SaveErrors::OperationAllocationFailed));
            return Return::Success(RuntimeSaveRequest(work_));
        } catch (...) {
            if (!admittedHere)
                return Return::Failure(MakeError(SaveErrors::LifecycleCallbackFailed));
            (void)Fail(*work_, MakeError(SaveErrors::LifecycleCallbackFailed));
            return Return::Success(RuntimeSaveRequest(work_));
        }
    }

    /** @copydoc RuntimeSaveSession::Load */
    Result<RuntimeSaveRequest> RuntimeSaveSession::Load(SaveOperationDescriptor operation,
                                                        std::shared_ptr<IRuntimeSaveRestoreSource> source) {
        using Return = Result<RuntimeSaveRequest>;
        if (auto valid = ValidateOwner(); valid.HasError())
            return Return::Failure(std::move(valid).ErrorValue());
        const MutationScope mutation(mutating_);
        if (operation.kind != SaveOperationKind::Load || !source)
            return Return::Failure(MakeError(SaveErrors::OperationInvalid));
        try {
            auto admitted = Admit(operation);
            if (admitted.HasError())
                return Return::Failure(std::move(admitted).ErrorValue());
            work_->source = std::move(source);
            (void)Dispatch(work_);
            return Return::Success(RuntimeSaveRequest(work_));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        }
    }

    /** @copydoc RuntimeSaveSession::Dispatch */
    Result<void> RuntimeSaveSession::Dispatch(const std::shared_ptr<Work> &work) {
        try {
            auto submitted = jobs_.SubmitResult({.parentCancellation = work->cancellation.Token(),
                                                 .operationId = work->handle.Id(),
                                                 .resource = workerLane_},
                                                [work](const CancellationToken &cancellation) {
                try {
                    return work->writer ? Write(*work, cancellation) : Read(*work, cancellation);
                } catch (const std::bad_alloc &) {
                    return Fail(*work, MakeError(SaveErrors::OperationAllocationFailed));
                } catch (...) {
                    return Fail(*work, MakeError(SaveErrors::LifecycleCallbackFailed));
                }
            });
            if (submitted.HasError())
                return Fail(*work, std::move(submitted).ErrorValue());
            work->job.emplace(std::move(submitted).Value());
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Fail(*work, MakeError(SaveErrors::OperationAllocationFailed));
        }
    }

    /** @copydoc RuntimeSaveSession::ReleaseBarrier */
    void RuntimeSaveSession::ReleaseBarrier() noexcept {
        if (!barrierPending_ || !work_)
            return;
        try {
            const auto state = barrier_.Snapshot();
            if (state.HasValue() && state.Value().state == SaveBarrierState::Pending)
                (void)barrier_.Cancel(work_->handle.Id());
            (void)barrier_.Acknowledge(work_->handle.Id());
            barrierPending_ = false;
        } catch (...) { /* Teardown cannot allow a host clock callback to escape. */
        }
    }

    /** @copydoc RuntimeSaveSession::Pump */
    Result<void> RuntimeSaveSession::Pump(const RuntimeSaveCaptureProvenance &provenance) {
        if (!work_)
            return Result<void>::Success();
        auto &work = *work_;
        if (work.handle.Snapshot()->IsTerminal()) {
            ReleaseBarrier();
            return Result<void>::Success();
        }
        if (!work.job && work.writer) {
            if (work.producer.ObserveCancellation() == SaveCancellationObservation::Cancelled) {
                ReleaseBarrier();
                return Result<void>::Success();
            }
            (void)work.producer.PublishProgress(SaveOperationStage::CapturingSnapshot, {});
            auto outcome =
                barrier_.CaptureAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation_, provenance, participants_, limits_);
            if (outcome.HasError())
                return Fail(work, std::move(outcome).ErrorValue());
            if (!outcome.Value().capture) {
                if (outcome.Value().barrier.state == SaveBarrierState::Pending)
                    return Result<void>::Success();
                return Fail(work, outcome.Value().error ? std::move(*outcome.Value().error) : MakeError(SaveErrors::LifecycleUnavailable));
            }
            work.capture = std::move(*outcome.Value().capture);
            ReleaseBarrier();
            Executor executor(work);
            auto drained = safePoints_->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation_, 1, executor);
            if (drained.HasError())
                return Fail(work, std::move(drained).ErrorValue());
            return Dispatch(work_);
        }
        if (!work.job)
            return Result<void>::Success();
        const auto job = work.job->Snapshot();
        if (!job || job->state == JobState::Queued || job->state == JobState::Running)
            return Result<void>::Success();
        if (!work.completionPublished) {
            const bool succeeded = job->state == JobState::Succeeded;
            auto completed = safePoints_->PublishWorkerCompletion(
                {work.handle.Id(), generation_, succeeded ? SaveWorkerCompletionOutcome::Succeeded : SaveWorkerCompletionOutcome::Failed,
                 succeeded ? std::optional<Error>{}
                           : std::optional<Error>{job->error.value_or(MakeError(SaveErrors::LifecycleCallbackFailed))}});
            work.completionPublished = true;
            if (completed.HasError())
                return Fail(work, std::move(completed).ErrorValue());
            if (!succeeded)
                return Fail(work, job->error.value_or(MakeError(SaveErrors::LifecycleCallbackFailed)));
        }
        if (!work.writer) {
            Executor executor(work);
            auto drained = safePoints_->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation_, 1, executor);
            if (drained.HasError())
                return Result<void>::Failure(std::move(drained).ErrorValue());
            const auto result = work.handle.Snapshot();
            if (result->terminalError)
                return Result<void>::Failure(*result->terminalError);
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSaveSession::CommitAtSafePoint */
    Result<void> RuntimeSaveSession::CommitAtSafePoint(RuntimePhase phase, SaveRuntimeGeneration current,
                                                       const RuntimeSaveCaptureProvenance &provenance) {
        if (auto valid = ValidateOwner(); valid.HasError())
            return valid;
        if (closed_)
            return Failure(SaveErrors::LifecycleUnavailable);
        if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
            return Failure(SaveErrors::SafePointInvalid);
        const MutationScope mutation(mutating_);
        if (current != generation_) {
            (void)RetireInternal();
            return Failure(SaveErrors::GenerationStale);
        }

        try {
            return Pump(provenance);
        } catch (const std::bad_alloc &) {
            return work_ ? Fail(*work_, MakeError(SaveErrors::OperationAllocationFailed)) : Failure(SaveErrors::OperationAllocationFailed);
        } catch (...) {
            return work_ ? Fail(*work_, MakeError(SaveErrors::LifecycleCallbackFailed)) : Failure(SaveErrors::LifecycleCallbackFailed);
        }
    }

    /** @copydoc RuntimeSaveSession::Retire */
    Result<void> RuntimeSaveSession::Retire() {
        if (auto valid = ValidateOwner(); valid.HasError())
            return valid;
        const MutationScope mutation(mutating_);
        return RetireInternal();
    }

    /** @copydoc RuntimeSaveSession::RetireInternal */
    Result<void> RuntimeSaveSession::RetireInternal() {
        closed_ = true;
        if (work_) {
            (void)work_->producer.RequestShutdownCancellation();
            work_->cancellation.RequestCancellation();
            if (work_->job)
                (void)work_->job->RequestCancel();
            (void)work_->producer.ObserveCancellation();
        }
        ReleaseBarrier();
        return safePoints_->BeginShutdown();
    }

    /** @copydoc RuntimeSaveSession::IsSettled */
    bool RuntimeSaveSession::IsSettled() const {
        if (owner_ != std::this_thread::get_id() || mutating_)
            return false;
        return Settled(work_);
    }

    /** @copydoc RuntimeSaveSession::Acknowledge */
    Result<void> RuntimeSaveSession::Acknowledge() {
        if (auto valid = ValidateOwner(); valid.HasError())
            return valid;
        const MutationScope mutation(mutating_);
        if (!work_ || !work_->handle.Snapshot()->IsTerminal() || !Settled(work_))
            return Failure(SaveErrors::OperationInProgress);
        ReleaseBarrier();
        // Prepare a successor before retiring records which may still await final worker polling.
        auto coordinator = SaveSafePointCoordinator::Create(generation_, 1);
        if (coordinator.HasError())
            return Result<void>::Failure(std::move(coordinator).ErrorValue());
        (void)safePoints_->BeginShutdown();
        safePoints_ = std::move(coordinator).Value();
        work_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
