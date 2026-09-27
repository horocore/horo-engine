#include "Horo/Release/ReleaseService.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Stable service admission failure shared by capacity boundaries. */
        [[nodiscard]] Error AdmissionRejected() {
            return MakeError(ReleaseErrors::PipelineAdmissionRejected);
        }

        /** @brief Preserves the active boundary when an unexpected exception escapes a worker. */
        void FailUnexpected(ReleaseJobTracker &tracker) {
            const ReleaseJobSnapshot snapshot = tracker.Snapshot();
            if (snapshot.terminal)
                return;
            Error error = MakeError(ReleaseErrors::PipelineStageException);
            if (snapshot.state == ReleaseJobState::Queued)
                (void)tracker.FailAdmission(std::move(error));
            else if (snapshot.activeStage) {
                const ReleaseStage stage = *snapshot.activeStage;
                const auto attempt = snapshot.stages[static_cast<std::size_t>(stage)].attempt;
                if (attempt)
                    (void)tracker.FailStage(stage, *attempt, std::move(error));
            } else
                (void)tracker.FailJob(std::move(error));
        }

        /** @brief Converts the authoritative release terminal to its operation projection. */
        [[nodiscard]] OperationUpdate TerminalOperation(const ReleaseJobSnapshot &snapshot) {
            OperationUpdate update;
            update.phase = "release";
            switch (snapshot.state) {
                case ReleaseJobState::Succeeded:
                    update.state = OperationState::Succeeded;
                    update.message = "Release candidate completed.";
                    update.progress = 1.0F;
                    break;
                case ReleaseJobState::Failed:
                    update.state = OperationState::Failed;
                    update.message = "Release pipeline failed.";
                    if (snapshot.terminal && std::holds_alternative<ReleaseFailed>(*snapshot.terminal))
                        update.error = std::get<ReleaseFailed>(*snapshot.terminal).cause;
                    break;
                case ReleaseJobState::Cancelled:
                    update.state = OperationState::Cancelled;
                    update.message = "Release job cancelled.";
                    break;
                default:
                    update.state = OperationState::Running;
                    break;
            }
            return update;
        }
    }  // namespace

    struct ReleaseService::CancellationSlot final {
        std::mutex mutex;
        std::weak_ptr<Record> record;
    };

    struct ReleaseService::CancellationGate final {
        std::mutex mutex;
        std::condition_variable idle;
        ReleaseService *owner{};
        std::size_t inFlight{};
    };

    struct ReleaseService::Record final {
        Record(const ReleaseJobId job, const ReleaseTargetId target, const ReleaseCandidateId candidate, const OperationId operation,
               ReleaseExecutionPlan plan, std::shared_ptr<CancellationSource> cancellation,
               std::shared_ptr<CancellationSlot> cancellationSlot)
            : tracker(job, target, operation, {plan.Request().signingSelected, plan.Request().publicationDestination.has_value()}),
              candidate(candidate), plan(std::move(plan)), cancellation(std::move(cancellation)),
              cancellationSlot(std::move(cancellationSlot)) {}

        ReleaseJobTracker tracker;
        ReleaseCandidateId candidate;
        ReleaseExecutionPlan plan;
        std::shared_ptr<CancellationSource> cancellation;
        std::shared_ptr<CancellationSlot> cancellationSlot;
        std::optional<JobHandle> worker;
        std::atomic<bool> terminalRecorded{false};
    };

    /** @copydoc ReleaseService::ReleaseService */
    ReleaseService::ReleaseService(OperationStore &operations, IReleasePreflightFactsProvider &facts, IReleaseWorkerFactory &workers,
                                   const ReleaseServiceConfig config)
        : operations_(operations), facts_(facts), workers_(workers), config_(config), jobs_(config.workers),
          cancellationGate_(std::make_shared<CancellationGate>()) {
        cancellationGate_->owner = this;
    }

    /** @copydoc ReleaseService::~ReleaseService */
    ReleaseService::~ReleaseService() {
        Shutdown();
    }

    /** @copydoc ReleaseService::RecordTerminal */
    void ReleaseService::RecordTerminal(const std::shared_ptr<Record> &record) {
        const ReleaseJobSnapshot snapshot = record->tracker.Snapshot();
        if (!snapshot.terminal || record->terminalRecorded.exchange(true))
            return;
        try {
            (void)operations_.Update(snapshot.operation, TerminalOperation(snapshot));
        } catch (...) {
            // The operation history sink is a projection; it cannot revoke the release terminal.
        }
        std::lock_guard lock(mutex_);
        if (activeCount_ > 0)
            --activeCount_;
        recent_.push_back(snapshot.id.value);
        if (recent_.size() > config_.recentCapacity) {
            records_.erase(recent_.front());
            recent_.pop_front();
        }
    }

    /** @copydoc ReleaseService::Submit */
    Result<ReleaseSubmission> ReleaseService::Submit(ReleaseExecutionPlan plan) {
        ReleaseJobId job;
        ReleaseTargetId target;
        ReleaseCandidateId candidate;
        {
            std::lock_guard lock(mutex_);
            if (closing_ || activeCount_ >= config_.activeCapacity || nextJob_ == 0 || nextTarget_ == 0 || nextCandidate_ == 0)
                return Result<ReleaseSubmission>::Failure(AdmissionRejected());
            job = ReleaseJobId{nextJob_++};
            target = ReleaseTargetId{nextTarget_++};
            candidate = ReleaseCandidateId{nextCandidate_++};
            ++activeCount_;
        }

        auto cancellation = std::make_shared<CancellationSource>();
        auto cancellationSlot = std::make_shared<CancellationSlot>();
        const auto cancellationGate = cancellationGate_;
        const auto operation = operations_.Begin(OperationDescriptor{.kind = OperationKind::Build,
                                                                     .title = "Release",
                                                                     .phase = "queued",
                                                                     .message = "Release job queued.",
                                                                     .cancellable = true,
                                                                     .requestCancel = [cancellation, cancellationSlot, cancellationGate] {
            cancellation->RequestCancellation();
            std::shared_ptr<Record> record;
            ReleaseService *owner = nullptr;
            {
                std::lock_guard lock(cancellationGate->mutex);
                owner = cancellationGate->owner;
                if (owner)
                    ++cancellationGate->inFlight;
            }
            if (!owner)
                return;
            {
                std::lock_guard lock(cancellationSlot->mutex);
                record = cancellationSlot->record.lock();
            }
            if (record) {
                try {
                    (void)record->tracker.RequestCancel();
                    if (record->tracker.Snapshot().terminal)
                        owner->RecordTerminal(record);
                } catch (...) {
                    // The owned worker or shutdown path still records the terminal.
                }
            }
            {
                std::lock_guard lock(cancellationGate->mutex);
                --cancellationGate->inFlight;
            }
            cancellationGate->idle.notify_all();
        }});
        if (!operation) {
            std::lock_guard lock(mutex_);
            --activeCount_;
            return Result<ReleaseSubmission>::Failure(AdmissionRejected());
        }

        auto record = std::make_shared<Record>(job, target, candidate, *operation, std::move(plan), cancellation, cancellationSlot);
        {
            std::lock_guard lock(mutex_);
            records_.emplace(job.value, record);
        }
        {
            std::lock_guard lock(cancellationSlot->mutex);
            cancellationSlot->record = record;
        }
        if (cancellation->Token().IsCancellationRequested())
            (void)record->tracker.RequestCancel();
        if (record->tracker.Snapshot().terminal) {
            RecordTerminal(record);
            return Result<ReleaseSubmission>::Success({job, target, *operation});
        }

        JobDescriptor descriptor;
        descriptor.parentCancellation = cancellation->Token();
        descriptor.operationId = *operation;
        auto submitted = jobs_.SubmitResult(descriptor, [this, record](const CancellationToken &token) {
            if (!record->tracker.Snapshot().terminal) {
                try {
                    auto worker = workers_.Create(record->plan);
                    if (worker.HasError())
                        (void)record->tracker.FailAdmission(worker.ErrorValue());
                    else if (!worker.Value())
                        (void)record->tracker.FailAdmission(MakeError(ReleaseErrors::PipelineOutputInvalid));
                    else if (!record->tracker.Snapshot().terminal) {
                        (void)operations_.Update(record->tracker.Snapshot().operation,
                                                 OperationUpdate{.state = OperationState::Running,
                                                                 .phase = "release",
                                                                 .message = "Release pipeline running."});
                        (void)ReleasePipelineExecutor{}.Execute(record->tracker, record->candidate, record->plan, facts_, *worker.Value(),
                                                                token, config_.pipeline);
                    }
                } catch (...) {
                    FailUnexpected(record->tracker);
                }
            }
            if (!record->tracker.Snapshot().terminal)
                FailUnexpected(record->tracker);
            RecordTerminal(record);
            const ReleaseJobSnapshot result = record->tracker.Snapshot();
            if (result.state == ReleaseJobState::Succeeded)
                return Result<void>::Success();
            if (result.state == ReleaseJobState::Cancelled)
                return JobCancelled();
            if (result.terminal && std::holds_alternative<ReleaseFailed>(*result.terminal))
                return Result<void>::Failure(std::get<ReleaseFailed>(*result.terminal).cause);
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
        });
        if (submitted.HasError()) {
            if (!record->tracker.Snapshot().terminal)
                (void)record->tracker.FailAdmission(submitted.ErrorValue());
            RecordTerminal(record);
            return Result<ReleaseSubmission>::Failure(std::move(submitted).ErrorValue());
        }
        record->worker = std::move(submitted).Value();
        return Result<ReleaseSubmission>::Success({job, target, *operation});
    }

    /** @copydoc ReleaseService::Query */
    std::optional<ReleaseJobSnapshot> ReleaseService::Query(const ReleaseJobId job) const {
        std::shared_ptr<Record> record;
        {
            std::lock_guard lock(mutex_);
            const auto found = records_.find(job.value);
            if (found == records_.end())
                return std::nullopt;
            record = found->second;
        }
        return record->tracker.Snapshot();
    }

    /** @copydoc ReleaseService::List */
    std::vector<ReleaseJobSnapshot> ReleaseService::List() const {
        std::vector<std::shared_ptr<Record>> records;
        {
            std::lock_guard lock(mutex_);
            records.reserve(records_.size());
            for (const auto &[id, record] : records_) {
                (void)id;
                records.push_back(record);
            }
        }
        std::vector<ReleaseJobSnapshot> snapshots;
        snapshots.reserve(records.size());
        for (const auto &record : records)
            snapshots.push_back(record->tracker.Snapshot());
        std::ranges::sort(snapshots, {}, [](const ReleaseJobSnapshot &snapshot) {
            return snapshot.id.value;
        });
        return snapshots;
    }

    /** @copydoc ReleaseService::Diagnostic */
    std::optional<ReleaseDiagnostic> ReleaseService::Diagnostic(const ReleaseJobId job, const ReleaseDiagnosticId diagnostic) const {
        std::shared_ptr<Record> record;
        {
            std::lock_guard lock(mutex_);
            const auto found = records_.find(job.value);
            if (found == records_.end())
                return std::nullopt;
            record = found->second;
        }
        return record->tracker.Diagnostic(diagnostic);
    }

    /** @copydoc ReleaseService::RequestCancel */
    Result<void> ReleaseService::RequestCancel(const ReleaseJobId job) {
        std::shared_ptr<Record> record;
        {
            std::lock_guard lock(mutex_);
            const auto found = records_.find(job.value);
            if (found == records_.end())
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
            record = found->second;
        }
        const ReleaseJobSnapshot before = record->tracker.Snapshot();
        if (before.state == ReleaseJobState::Cancelled || before.state == ReleaseJobState::Cancelling)
            return Result<void>::Success();
        if (before.terminal)
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
        if (!operations_.RequestCancel(before.operation))
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineTransitionInvalid));
        if (record->tracker.Snapshot().terminal)
            RecordTerminal(record);
        return Result<void>::Success();
    }

    /** @copydoc ReleaseService::Shutdown */
    void ReleaseService::Shutdown() {
        std::vector<ReleaseJobId> active;
        {
            std::lock_guard lock(mutex_);
            if (closing_)
                return;
            closing_ = true;
            for (const auto &[id, record] : records_) {
                if (!record->tracker.Snapshot().terminal)
                    active.push_back(ReleaseJobId{id});
            }
        }
        for (const ReleaseJobId id : active)
            (void)RequestCancel(id);
        jobs_.Shutdown(ShutdownPolicy::Cancel);
        {
            std::unique_lock lock(cancellationGate_->mutex);
            cancellationGate_->owner = nullptr;
            cancellationGate_->idle.wait(lock, [this] {
                return cancellationGate_->inFlight == 0;
            });
        }
        for (const ReleaseJobId id : active) {
            std::shared_ptr<Record> record;
            {
                std::lock_guard lock(mutex_);
                const auto found = records_.find(id.value);
                if (found != records_.end())
                    record = found->second;
            }
            if (record)
                RecordTerminal(record);
        }
    }
}  // namespace Horo::Release
