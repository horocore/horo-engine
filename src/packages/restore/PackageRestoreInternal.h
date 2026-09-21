#pragma once

#include "Horo/Packages/PackageRestore.h"
#include "Horo/Packages/PackageRestoreErrors.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace Horo::Packages::RestoreInternal {
    struct MutexHolder {
        [[nodiscard]] std::mutex &Mutex() const noexcept {
            return mutex_;
        }

    private:
        mutable std::mutex mutex_;
    };

    struct Completion final : private MutexHolder {
        using MutexHolder::Mutex;

        PackageRestorePhase phase{PackageRestorePhase::ValidatingRequest};
        float progress{};
        std::size_t completedPackages{};
        std::size_t totalPackages{};
        std::size_t cacheHits{};
        std::size_t downloadedPackages{};
        std::string currentPackage;
        std::uint64_t revision{};
        std::optional<PackageRestoreGraph> candidate;
        std::optional<Error> error;
    };

    struct RestoreContext final {
        DurableFileSystem &files;
        PackageCacheStore &cache;
        IPackageRestoreSource *source{};
        PackagePublisherVerificationService *publisherVerification{};
        const PackageRestoreLimits &limits;
    };

    [[nodiscard]] bool ValidLimits(const PackageRestoreLimits &limits) noexcept;
    [[nodiscard]] Result<void> ValidateStartRequest(const PackageRestoreRequest &request, const PackageRestoreLimits &limits);
    [[nodiscard]] Result<void> RunRestoreJob(const RestoreContext &context, const PackageRestoreRequest &request,
                                             PackageRestoreOperationId operation, const CancellationToken &cancellation,
                                             const std::shared_ptr<Completion> &completion);
}  // namespace Horo::Packages::RestoreInternal

namespace Horo::Packages {
    struct PackageRestoreService::Impl final : private RestoreInternal::MutexHolder {
        using RestoreInternal::MutexHolder::Mutex;

        struct Operation final {
            PackageRestoreProgressSnapshot snapshot;
            PackageRestoreRequest request;
            CancellationSource cancellation;
            std::shared_ptr<RestoreInternal::Completion> completion;
            std::optional<JobHandle> job;
            std::uint64_t observedCompletionRevision{};
        };

        JobSystem &jobs;
        DurableFileSystem &files;
        PackageCacheStore &cache;
        IPackageRestoreSource *source{};
        PackagePublisherVerificationService *publisherVerification{};
        PackageRestoreLimits limits;
        std::optional<Operation> operation;
        std::shared_ptr<const PackageRestoreGraph> activeGraph;
        PackageRestoreServiceState serviceState;
        std::uint64_t nextOperation{1U};
        bool shutdown{};

        Impl(JobSystem &jobsValue, DurableFileSystem &filesValue, PackageCacheStore &cacheValue, IPackageRestoreSource *sourceValue,
             PackagePublisherVerificationService *publisherVerificationValue, PackageRestoreLimits limitsValue)
            : jobs(jobsValue), files(filesValue), cache(cacheValue), source(sourceValue), publisherVerification(publisherVerificationValue),
              limits(std::move(limitsValue)) {}

        [[nodiscard]] RestoreInternal::RestoreContext Context() const noexcept {
            return RestoreInternal::RestoreContext{files, cache, source, publisherVerification, limits};
        }

        [[nodiscard]] Result<JobHandle> SubmitRestoreJob(const Operation &operationState, const PackageRestoreOperationId id) const {
            const PackageRestoreRequest requestCopy = operationState.request;
            const CancellationToken cancellation = operationState.cancellation.Token();
            const auto completion = operationState.completion;
            return jobs.SubmitResult(JobDescriptor{.parentCancellation = cancellation},
                                     [this, completion, requestCopy, id](const CancellationToken &jobCancellation) {
                return RestoreInternal::RunRestoreJob(Context(), requestCopy, id, jobCancellation, completion);
            });
        }

        void RefreshProgress(Operation &operationState) {
            std::lock_guard completionLock(operationState.completion->Mutex());
            if (operationState.observedCompletionRevision == operationState.completion->revision)
                return;
            operationState.snapshot.phase = operationState.completion->phase;
            operationState.snapshot.progress = std::max(operationState.snapshot.progress, operationState.completion->progress);
            operationState.snapshot.completedPackages = operationState.completion->completedPackages;
            operationState.snapshot.totalPackages = operationState.completion->totalPackages;
            operationState.snapshot.cacheHits = operationState.completion->cacheHits;
            operationState.snapshot.downloadedPackages = operationState.completion->downloadedPackages;
            operationState.snapshot.currentPackage = operationState.completion->currentPackage;
            operationState.observedCompletionRevision = operationState.completion->revision;
            operationState.snapshot.revision = ++serviceState.revision;
        }

        [[nodiscard]] std::optional<JobSnapshot> CompletedJob(const Operation &operationState) const {
            if (!operationState.job.has_value())
                return std::nullopt;
            const JobSnapshot job = jobs.Query(operationState.job->Id());
            if (job.state == JobState::Queued || job.state == JobState::Running)
                return std::nullopt;
            return job;
        }

        void CancelAndWait(Operation &operationState) const {
            operationState.cancellation.RequestCancellation();
            if (!operationState.job.has_value())
                return;
            static_cast<void>(jobs.RequestCancel(operationState.job->Id()));
            static_cast<void>(operationState.job->Wait());
        }

        void FinalizeCancelled(Operation &operationState) {
            operationState.snapshot.phase = PackageRestorePhase::Cancelled;
            operationState.snapshot.outcome = PackageRestoreOutcome::Cancelled;
            operationState.snapshot.progress = 1.0F;
            operationState.snapshot.currentPackage.clear();
            operationState.snapshot.diagnostic = MakeError(PackageRestoreErrors::Cancelled);
            operationState.snapshot.revision = ++serviceState.revision;
            serviceState.activeOperation.reset();
            serviceState.lifecycle = shutdown ? PackageRestoreLifecycleState::Closed : PackageRestoreLifecycleState::Ready;
        }

        void FinalizeFailure(Operation &operationState, const JobSnapshot &job, std::optional<Error> error) {
            operationState.snapshot.phase = PackageRestorePhase::Failed;
            operationState.snapshot.outcome = PackageRestoreOutcome::Failed;
            operationState.snapshot.progress = 1.0F;
            operationState.snapshot.currentPackage.clear();
            if (error.has_value())
                operationState.snapshot.diagnostic = std::move(error);
            else if (job.error.has_value())
                operationState.snapshot.diagnostic = job.error;
            else
                operationState.snapshot.diagnostic =
                    MakeError(PackageRestoreErrors::InvalidInput, "Restore worker completed without a graph candidate.");
            operationState.snapshot.revision = ++serviceState.revision;
            serviceState.activeOperation.reset();
            serviceState.lifecycle = shutdown ? PackageRestoreLifecycleState::Closed : PackageRestoreLifecycleState::RecoverableFailure;
        }

        void FinalizeReady(Operation &operationState) {
            operationState.snapshot.phase = PackageRestorePhase::Completed;
            operationState.snapshot.outcome = PackageRestoreOutcome::Ready;
            operationState.snapshot.progress = 1.0F;
            operationState.snapshot.currentPackage.clear();
            operationState.snapshot.revision = ++serviceState.revision;
            serviceState.activeOperation.reset();
            serviceState.lifecycle = shutdown ? PackageRestoreLifecycleState::Closed : PackageRestoreLifecycleState::Ready;
        }
    };
}  // namespace Horo::Packages
