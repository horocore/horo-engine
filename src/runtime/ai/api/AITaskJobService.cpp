#include "Horo/AI/AITaskJobService.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::AI {
    /** @brief Immutable owned pair; declaration order pins code through work destruction. */
    struct AiTaskJobLease::State final {
        std::shared_ptr<const void> image;
        std::shared_ptr<const IAiTaskJob> work;
    };

    /** @copydoc AiTaskJobLease::AiTaskJobLease */
    AiTaskJobLease::AiTaskJobLease(std::shared_ptr<const void> imageLease, std::shared_ptr<const IAiTaskJob> taskWork) {
        // The local pair preserves work-before-image destruction even if allocation fails.
        State captured{std::move(imageLease), std::move(taskWork)};
        state_ = std::make_shared<const State>(std::move(captured));
    }

    /** @copydoc AiTaskJobLease::IsValid */
    bool AiTaskJobLease::IsValid() const noexcept {
        return state_ && state_->image && state_->work;
    }

    /** @copydoc AiTaskJobLease::Run */
    Result<void> AiTaskJobLease::Run(const AiTaskOperationContext &operation, const CancellationToken &cancellation) const {
        return state_->work->Run(operation, cancellation);
    }

    struct AiTaskJobService::Entry final {
        JobHandle job;
        AiTaskContinuation continuation;
        std::optional<AiTaskTerminalResult> pending;
        AiTaskCancellationReason cancellation{AiTaskCancellationReason::Requested};
    };

    /** @copydoc AiTaskJobService::AiTaskJobService */
    AiTaskJobService::AiTaskJobService(ConstructionKey, JobSystem &jobs, const std::size_t capacity) : jobs_(jobs), entries_(capacity) {}

    /** @copydoc AiTaskJobService::Create */
    Result<std::unique_ptr<AiTaskJobService>> AiTaskJobService::Create(JobSystem &jobs, const std::size_t capacity) {
        if (capacity == 0 || capacity > HardCapacity)
            return Result<std::unique_ptr<AiTaskJobService>>::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        try {
            return Result<std::unique_ptr<AiTaskJobService>>::Success(
                std::make_unique<AiTaskJobService>(ConstructionKey{}, jobs, capacity));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<AiTaskJobService>>::Failure(MakeError(AIErrors::BehaviorTreeStorageUnavailable));
        }
    }

    /** @copydoc AiTaskJobService::Start */
    Result<void> AiTaskJobService::Start(AiTaskContinuation continuation, AiTaskJobLease lease) {
        if (closed_ || !continuation.IsCurrent() || !lease.IsValid())
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        Entry *free = nullptr;
        for (auto &entry : entries_) {
            if (entry.job.Id() == 0)
                free = &entry;
            else if (entry.continuation.Operation().task == continuation.Operation().task)
                return Result<void>::Failure(MakeError(AIErrors::TaskTransitionInvalid));
        }
        if (!free)
            return Result<void>::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        const auto operation = continuation.Operation();
        auto submitted = jobs_.SubmitResult({.parentCancellation = operation.cancellation},
                                            [lease = std::move(lease), operation](const CancellationToken &cancellation) {
            return lease.Run(operation, cancellation);
        });
        if (submitted.HasError())
            return Result<void>::Failure(submitted.ErrorValue());
        free->job = std::move(submitted).Value();
        free->continuation = std::move(continuation);
        free->cancellation = AiTaskCancellationReason::Requested;
        return Result<void>::Success();
    }

    /** @copydoc AiTaskJobService::Cancel */
    Result<void> AiTaskJobService::Cancel(const TaskHandle task, const AiTaskCancellationReason reason) {
        if (!task.IsValid() || reason >= AiTaskCancellationReason::Count)
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        for (auto &entry : entries_) {
            if (entry.job.Id() != 0 && entry.continuation.Operation().task == task) {
                entry.cancellation = reason;
                return entry.job.RequestCancel();
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc AiTaskJobService::Pump */
    void AiTaskJobService::Pump() {
        for (auto &entry : entries_) {
            if (entry.job.Id() == 0)
                continue;
            if (!entry.pending) {
                const auto snapshot = entry.job.Snapshot();
                if (!snapshot || !snapshot->terminalResult)
                    continue;
                const auto &terminal = *snapshot->terminalResult;
                if (terminal.state == JobState::Succeeded)
                    entry.pending.emplace(AiTaskTerminalResult{.state = AiTaskState::Succeeded});
                else if (terminal.state == JobState::Cancelled)
                    entry.pending.emplace(AiTaskTerminalResult{.state = AiTaskState::Cancelled, .cancellationReason = entry.cancellation});
                else
                    entry.pending.emplace(
                        AiTaskTerminalResult{.state = AiTaskState::Failed,
                                             .failure = AiTaskFailureDetail{AiTaskFailureKind::Execution, *terminal.error}});
            }
            const auto published = entry.continuation.PublishTerminal(*entry.pending);
            if (published.HasValue() && published.Value() != AiTaskPublicationDisposition::Contended)
                entry = {};
        }
    }

    /** @copydoc AiTaskJobService::CancelAgent */
    void AiTaskJobService::CancelAgent(const AgentHandle agent, const AiTaskCancellationReason reason) noexcept {
        for (auto &entry : entries_) {
            if (entry.job.Id() != 0 && entry.continuation.Operation().agent == agent) {
                entry.cancellation = reason;
                static_cast<void>(entry.job.RequestCancel());
            }
        }
    }

    /** @copydoc AiTaskJobService::PendingCount */
    std::size_t AiTaskJobService::PendingCount(const AgentHandle agent) const noexcept {
        return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(), [agent](const Entry &entry) {
            return entry.job.Id() != 0 && entry.continuation.Operation().agent == agent;
        }));
    }

    /** @copydoc AiTaskJobService::Shutdown */
    void AiTaskJobService::Shutdown() noexcept {
        if (closed_)
            return;
        closed_ = true;
        for (auto &entry : entries_) {
            if (entry.job.Id() != 0) {
                entry.cancellation = AiTaskCancellationReason::OwnerShutdown;
                static_cast<void>(entry.job.RequestCancel());
            }
        }
    }

    /** @copydoc AiTaskJobService::PendingCount */
    std::size_t AiTaskJobService::PendingCount() const noexcept {
        std::size_t count{};
        for (const auto &entry : entries_)
            if (entry.job.Id() != 0)
                ++count;
        return count;
    }

    /** @copydoc AiTaskJobService::~AiTaskJobService */
    AiTaskJobService::~AiTaskJobService() {
        Shutdown();
    }
}  // namespace Horo::AI
