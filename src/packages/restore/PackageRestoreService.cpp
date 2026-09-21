#include "Horo/Packages/PackageRestore.h"
#include "Horo/Packages/PackageRestoreErrors.h"
#include "PackageRestoreInternal.h"

#include <exception>
#include <new>
#include <utility>

namespace Horo::Packages {
    /** @brief Synchronizes one worker projection and atomically publishes a complete candidate graph. */
    void PackageRestoreService::RefreshLocked() const {
        if (!state_->operation.has_value())
            return;
        auto &operation = *state_->operation;
        if (operation.snapshot.outcome != PackageRestoreOutcome::Running)
            return;

        state_->RefreshProgress(operation);
        const auto completedJob = state_->CompletedJob(operation);
        if (!completedJob.has_value())
            return;
        const JobSnapshot job = *completedJob;

        std::optional<PackageRestoreGraph> candidate;
        std::optional<Error> error;
        {
            std::lock_guard completionLock(operation.completion->Mutex());
            candidate = std::move(operation.completion->candidate);
            error = operation.completion->error;
        }

        if (const bool cancelled = operation.cancellation.Token().IsCancellationRequested() || job.state == JobState::Cancelled;
            cancelled) {
            state_->FinalizeCancelled(operation);
            return;
        }
        if (!candidate.has_value()) {
            state_->FinalizeFailure(operation, job, std::move(error));
            return;
        }

        try {
            state_->activeGraph = std::make_shared<const PackageRestoreGraph>(std::move(*candidate));
        } catch (const std::bad_alloc &) {
            state_->FinalizeFailure(operation, job,
                                    MakeError(PackageRestoreErrors::ResourceLimit,
                                              "The restored package graph could not be retained within host memory."));
            return;
        }
        state_->FinalizeReady(operation);
    }

    /** @copydoc PackageRestoreService::Create */
    Result<PackageRestoreService> PackageRestoreService::Create(JobSystem &jobs, DurableFileSystem &files, PackageCacheStore &cache,
                                                                IPackageRestoreSource *source,
                                                                PackagePublisherVerificationService *publisherVerification,
                                                                PackageRestoreLimits limits) {
        if (!RestoreInternal::ValidLimits(limits))
            return Result<PackageRestoreService>::Failure(MakeError(PackageRestoreErrors::InvalidInput));
        return Result<PackageRestoreService>::Success(
            PackageRestoreService{std::make_unique<Impl>(jobs, files, cache, source, publisherVerification, std::move(limits))});
    }

    /** @copydoc PackageRestoreService::PackageRestoreService */
    PackageRestoreService::PackageRestoreService(std::unique_ptr<Impl> state) : state_(std::move(state)) {}

    PackageRestoreService::PackageRestoreService(PackageRestoreService &&) noexcept = default;
    PackageRestoreService &PackageRestoreService::operator=(PackageRestoreService &&) noexcept = default;

    PackageRestoreService::~PackageRestoreService() noexcept {
        try {
            static_cast<void>(Shutdown());
        } catch (...) {
            // A failed shutdown could leave a worker's context pointing at the implementation being destroyed.
            std::terminate();
        }
    }

    /** @copydoc PackageRestoreService::Start */
    Result<PackageRestoreOperationHandle> PackageRestoreService::Start(const PackageRestoreRequest &request) {
        std::lock_guard lock(state_->Mutex());
        if (state_->shutdown)
            return Result<PackageRestoreOperationHandle>::Failure(MakeError(PackageRestoreErrors::LifecycleClosed));
        if (state_->operation.has_value() && state_->operation->snapshot.outcome == PackageRestoreOutcome::Running)
            return Result<PackageRestoreOperationHandle>::Failure(MakeError(PackageRestoreErrors::Busy));
        if (auto valid = RestoreInternal::ValidateStartRequest(request, state_->limits); valid.HasError())
            return Result<PackageRestoreOperationHandle>::Failure(valid.ErrorValue());

        state_->operation.reset();
        const PackageRestoreOperationId id{state_->nextOperation++};
        auto completion = std::make_shared<RestoreInternal::Completion>();
        completion->currentPackage.clear();
        PackageRestoreProgressSnapshot snapshot{.operationId = id,
                                                .phase = PackageRestorePhase::ValidatingRequest,
                                                .outcome = PackageRestoreOutcome::Running,
                                                .progress = 0.0F,
                                                .offline = request.mode == PackageRestoreMode::Offline};
        Impl::Operation operation{.snapshot = std::move(snapshot),
                                  .request = request,
                                  .cancellation = CancellationSource{request.cancellation},
                                  .completion = completion};
        auto submitted = state_->SubmitRestoreJob(operation, id);
        if (submitted.HasError())
            return Result<PackageRestoreOperationHandle>::Failure(submitted.ErrorValue());
        operation.job.emplace(std::move(submitted).Value());
        state_->operation.emplace(std::move(operation));
        state_->serviceState.lifecycle = PackageRestoreLifecycleState::Running;
        state_->serviceState.activeOperation = id;
        ++state_->serviceState.attempts;
        ++state_->serviceState.revision;
        return Result<PackageRestoreOperationHandle>::Success(PackageRestoreOperationHandle{id});
    }

    /** @copydoc PackageRestoreService::Query */
    std::optional<PackageRestoreProgressSnapshot> PackageRestoreService::Query(const PackageRestoreOperationId operation) const {
        std::lock_guard lock(state_->Mutex());
        if (!state_->operation.has_value() || state_->operation->snapshot.operationId != operation)
            return std::nullopt;
        RefreshLocked();
        return state_->operation->snapshot;
    }

    /** @copydoc PackageRestoreService::RequestCancel */
    Result<void> PackageRestoreService::RequestCancel(const PackageRestoreOperationId operation) {
        std::lock_guard lock(state_->Mutex());
        if (state_->shutdown)
            return Result<void>::Failure(MakeError(PackageRestoreErrors::LifecycleClosed));
        if (!state_->operation.has_value() || state_->operation->snapshot.operationId != operation)
            return Result<void>::Failure(MakeError(PackageRestoreErrors::InvalidInput, "Package restore operation was not found."));
        if (state_->operation->snapshot.outcome != PackageRestoreOutcome::Running)
            return Result<void>::Success();
        state_->operation->cancellation.RequestCancellation();
        if (state_->operation->job.has_value())
            static_cast<void>(state_->jobs.RequestCancel(state_->operation->job->Id()));
        return Result<void>::Success();
    }

    /** @copydoc PackageRestoreService::Pump */
    void PackageRestoreService::Pump() const {
        std::lock_guard lock(state_->Mutex());
        RefreshLocked();
    }

    /** @copydoc PackageRestoreService::ActiveGraph */
    std::shared_ptr<const PackageRestoreGraph> PackageRestoreService::ActiveGraph() const {
        std::lock_guard lock(state_->Mutex());
        return state_->activeGraph;
    }

    /** @copydoc PackageRestoreService::State */
    PackageRestoreServiceState PackageRestoreService::State() const {
        std::lock_guard lock(state_->Mutex());
        auto result = state_->serviceState;
        result.hasActiveGraph = state_->activeGraph != nullptr;
        return result;
    }

    /** @copydoc PackageRestoreService::Shutdown */
    Result<void> PackageRestoreService::Shutdown() {
        if (!state_)
            return Result<void>::Success();
        std::lock_guard lock(state_->Mutex());
        if (state_->shutdown)
            return Result<void>::Success();
        state_->shutdown = true;
        if (state_->operation.has_value() && state_->operation->snapshot.outcome == PackageRestoreOutcome::Running) {
            state_->CancelAndWait(*state_->operation);
            RefreshLocked();
        }
        state_->serviceState.lifecycle = PackageRestoreLifecycleState::Closed;
        ++state_->serviceState.revision;
        return Result<void>::Success();
    }
}  // namespace Horo::Packages
