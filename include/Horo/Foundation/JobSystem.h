#pragma once

/**
 * @file JobSystem.h
 * @brief Bounded job scheduling, durable handles, authoritative records and structured child work.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Time.h"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Horo {
    using JobId = std::uint64_t;

    /** @brief Process-local identity correlating structured child work without creating a user-facing operation. */
    struct TaskGroupId final {
        std::uint64_t value{};

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const TaskGroupId &) const noexcept = default;
    };

    /** @brief Lifecycle state of an accepted job. */
    enum class JobState : std::uint8_t {
        Queued,
        Running,
        Succeeded,
        Failed,
        Cancelled,
    };

    /** @brief Inline bounded progress phase name that never allocates while a job reports progress. */
    class JobProgressPhase final {
    public:
        static constexpr std::size_t MaximumBytes = 128;

        /** @brief Constructs the empty initial phase. */
        JobProgressPhase() = default;

        /** @brief Returns the exact admitted UTF-8 bytes. @return Borrowed view owned by this value. */
        [[nodiscard]] std::string_view View() const noexcept {
            return {characters_.data(), size_};
        }

        /** @brief Checks whether no phase has been published. @return True only for the empty initial phase. */
        [[nodiscard]] bool Empty() const noexcept {
            return size_ == 0;
        }

        [[nodiscard]] friend bool operator==(const JobProgressPhase &left, const std::string_view right) noexcept {
            return left.View() == right;
        }

        [[nodiscard]] constexpr auto operator<=>(const JobProgressPhase &) const noexcept = default;

    private:
        friend class JobExecutionContext;
        explicit JobProgressPhase(std::string_view value) noexcept;

        std::array<char, MaximumBytes> characters_{};
        std::uint8_t size_{};
    };

    /** @brief Monotonic progress within one bounded named phase. */
    struct JobProgress final {
        JobProgressPhase phase;
        std::optional<float> value;
    };

    /** @brief Queue, execution and terminal timestamps from the scheduler monotonic clock. */
    struct JobTiming final {
        std::chrono::steady_clock::time_point submittedAt{};
        std::optional<std::chrono::steady_clock::time_point> startedAt;
        std::optional<std::chrono::steady_clock::time_point> finishedAt;
    };

    /** @brief Immutable typed result installed by the job's first terminal transition; only success has no error. */
    struct JobTerminalResult final {
        JobState state{JobState::Failed};
        std::optional<Error> error;
    };

    /** @brief Snapshot of an accepted job's immutable identity and current durable state. */
    struct JobSnapshot {
        JobId id = 0;
        JobState state = JobState::Queued;
        JobProgress progress;
        JobTiming timing;
        std::optional<JobTerminalResult> terminalResult;
        std::optional<OperationId> operationId;
        TaskGroupId taskGroupId;
        std::optional<ConfigurationRevision> configurationRevision;
        std::optional<Error> error;
    };

    /** @brief Bounded owned view of all retained active and recent terminal job records. */
    struct JobStoreSnapshot final {
        std::uint64_t revision{};
        std::size_t terminalCapacity{};
        std::uint64_t droppedTerminalCount{};
        std::vector<JobSnapshot> jobs;
    };

    /** @brief Result-returning unit of scheduled work. */
    using JobFunction = std::function<Result<void>(const CancellationToken &)>;

    /**
     * @brief Explicitly acknowledges cooperative cancellation from a job callback.
     * @param cause Optional typed reason retained beneath the job cancellation identity.
     * @return The cancellation result to return from a job callback.
     */
    [[nodiscard]] Result<void> JobCancelled(std::optional<Error> cause = std::nullopt);

    /** @brief Checks the exact Foundation cancellation identity, including its owning domain. @return True only for job.cancelled. */
    [[nodiscard]] bool IsJobCancelled(const Error &error) noexcept;

    /** @brief Submission metadata retained by the job system. */
    struct JobDescriptor {
        CancellationToken parentCancellation;                  /**< Optional parent operation cancellation. */
        std::optional<OperationId> operationId;                /**< Optional explicit application operation correlation only. */
        TaskGroupId taskGroupId;                               /**< Optional structured-work correlation, replaced by TaskGroup. */
        std::optional<ConfigurationSnapshotRef> configuration; /**< Explicit immutable submission configuration. */
    };

    /** @brief Fixed scheduling limits for one JobSystem instance. */
    struct JobSystemConfig {
        std::size_t workerCount = 1;
        std::size_t maxQueuedJobs = 1024;
        std::size_t maxRetainedTerminalJobs = 1024;
    };

    /** @brief Defines how queued work is treated during shutdown. */
    enum class ShutdownPolicy : std::uint8_t {
        Drain,
        Cancel,
    };

    /** @brief Selects the caller-affinity rule for one bounded wait. */
    enum class WaitPolicy : std::uint8_t {
        WorkerOnly,
        MainThreadPumpAllowed,
        ForbiddenOnOwnerThread,
        OwnerThreadBlockAllowed,
    };

    /** @brief Explicit finite policy for a bounded job wait. */
    struct JoinOptions {
        WaitPolicy waitPolicy{WaitPolicy::WorkerOnly}; /**< Caller-affinity rule checked before observing completion. */
        Duration timeout{};                            /**< Maximum duration of this wait. */
    };

    struct JobRecord;

    /** @brief Read-only submission-captured state and progress control supplied to one callback. */
    class JobExecutionContext final {
    public:
        JobExecutionContext(const JobExecutionContext &) = delete;
        JobExecutionContext &operator=(const JobExecutionContext &) = delete;
        JobExecutionContext(JobExecutionContext &&) = delete;
        JobExecutionContext &operator=(JobExecutionContext &&) = delete;

        /** @brief Returns this job's cooperative cancellation ancestry. */
        [[nodiscard]] const CancellationToken &Cancellation() const noexcept;
        /** @brief Returns the explicitly captured immutable configuration, when the work is configuration-dependent. */
        [[nodiscard]] const std::optional<ConfigurationSnapshotRef> &Configuration() const noexcept;
        /** @brief Returns explicit application-operation correlation without creating an OperationStore record. */
        [[nodiscard]] std::optional<OperationId> Operation() const noexcept;
        /** @brief Returns the structured task-group correlation, or an invalid ID for independent work. */
        [[nodiscard]] TaskGroupId Group() const noexcept;
        /**
         * @brief Publishes bounded progress, monotonic within one phase.
         * @param phase Non-empty phase name of at most 128 UTF-8 bytes.
         * @param value Optional normalized progress in [0, 1]. A new phase may restart progress.
         * @return Success, or a typed validation/lifecycle error without mutating the record.
         */
        [[nodiscard]] Result<void> UpdateProgress(std::string_view phase, std::optional<float> value = std::nullopt) const;

    private:
        friend class JobSystem;
        friend struct JobRecord;
        explicit JobExecutionContext(std::shared_ptr<JobRecord> record);
        std::shared_ptr<JobRecord> record_;
        CancellationToken cancellation_;
    };

    /** @brief Result-returning unit of work with immutable submission context and progress control. */
    using ContextJobFunction = std::function<Result<void>(const JobExecutionContext &)>;

    /** @brief Move-only reference to a durable accepted job record. */
    class JobHandle {
    public:
        JobHandle() = default;
        JobHandle(const JobHandle &) = delete;
        JobHandle &operator=(const JobHandle &) = delete;
        JobHandle(JobHandle &&) noexcept = default;
        JobHandle &operator=(JobHandle &&) noexcept = default;

        /** @brief Waits until the job reaches its single terminal state. @return Success, the retained failure, or job.cancelled. */
        [[nodiscard]] Result<void> Wait() const;
        /**
         * @brief Waits under a finite affinity policy, optionally helping only this exact queued record.
         * @param options Caller-affinity rule and maximum wait duration. `MainThreadPumpAllowed` and `WorkerOnly` may claim
         * this record for inline execution; `OwnerThreadBlockAllowed` waits without pumping. Unrelated queued jobs are never pumped.
         * @return Terminal job result, or a typed forbidden, deadlock-risk or timeout error. Policy validation occurs even
         * if the job is already terminal. A timeout does not change the job lifecycle, so the handle remains safely retryable.
         */
        [[nodiscard]] Result<void> Wait(const JoinOptions &options) const;
        /** @brief Returns the stable identifier assigned at successful submission. */
        [[nodiscard]] JobId Id() const noexcept;
        /** @brief Returns an owned consistent snapshot even after bounded store eviction. */
        [[nodiscard]] std::optional<JobSnapshot> Snapshot() const;
        /** @brief Requests cooperative cancellation through this durable record lease; terminal work is unchanged. */
        [[nodiscard]] Result<void> RequestCancel() const;

    private:
        friend class JobSystem;
        friend class TaskGroup;

        explicit JobHandle(std::shared_ptr<JobRecord> record) : m_record(std::move(record)) {}

        std::shared_ptr<JobRecord> m_record;
    };

    /** @brief Bounded worker queue with owned, joinable worker threads. */
    class JobSystem {
    public:
        explicit JobSystem(JobSystemConfig config = {});
        ~JobSystem();
        JobSystem(const JobSystem &) = delete;
        JobSystem &operator=(const JobSystem &) = delete;
        JobSystem(JobSystem &&) = delete;
        JobSystem &operator=(JobSystem &&) = delete;

        /** @brief Queues work or returns a typed overload error without creating a job record. */
        [[nodiscard]] Result<JobHandle> Submit(JobDescriptor descriptor, std::function<void(const CancellationToken &)> work) const;
        /**
         * @brief Queues result-returning work without translating typed failures into exceptions.
         * @param descriptor Submission metadata including optional parent cancellation.
         * @param work Owned callback executed by one worker.
         * @return Move-only accepted-job handle or a typed admission failure.
         */
        [[nodiscard]] Result<JobHandle> SubmitResult(JobDescriptor descriptor, JobFunction work) const;
        /**
         * @brief Queues context-aware work after freezing all descriptor and diagnostic context.
         * @param descriptor Explicit cancellation, correlation and configuration inputs.
         * @param work Owned callback receiving read-only captured context and progress control.
         * @return Move-only accepted-job handle or a typed admission failure. Rejection creates no record.
         */
        [[nodiscard]] Result<JobHandle> SubmitContext(JobDescriptor descriptor, ContextJobFunction work) const;
        /** @brief Requests cooperative cancellation; a still-queued job becomes terminal immediately. */
        [[nodiscard]] Result<void> RequestCancel(JobId id) const;
        /** @brief Returns the latest state for an accepted job. */
        [[nodiscard]] JobSnapshot Query(JobId id) const;
        /** @brief Finds a retained record, distinguishing eviction or unknown identity from a queued job. */
        [[nodiscard]] std::optional<JobSnapshot> Find(JobId id) const;
        /** @brief Returns a bounded store snapshot only when its authoritative revision changed. */
        [[nodiscard]] std::optional<JobStoreSnapshot> SnapshotIfChanged(std::uint64_t knownRevision) const;
        /** @brief Returns the immutable worker count configured for this scheduler. */
        [[nodiscard]] std::size_t WorkerCount() const noexcept;
        /** @brief Stops submissions, then drains or cooperatively cancels work and joins all workers. */
        void Shutdown(ShutdownPolicy policy) const;

    private:
        struct State;
        static void RunWorker(const std::shared_ptr<State> &state);
        std::shared_ptr<State> m_state;
    };

    /** @brief Failure propagation policy for one structured child-work scope. */
    enum class TaskGroupFailurePolicy : std::uint8_t {
        FailFast,
        CollectAll,
    };

    /** @brief Final aggregate outcome after all accepted children have been joined. */
    enum class TaskGroupOutcome : std::uint8_t {
        Completed,
        Failed,
        Cancelled,
    };

    /** @brief Operation-owned structured concurrency scope over an injected JobSystem. */
    class TaskGroup {
    public:
        /**
         * @brief Creates a child-work scope.
         * @param jobs Job system that outlives this group.
         * @param policy Child failure propagation policy.
         * @param parentCancellation Optional parent operation cancellation.
         */
        explicit TaskGroup(JobSystem &jobs, TaskGroupFailurePolicy policy = TaskGroupFailurePolicy::FailFast,
                           CancellationToken parentCancellation = {});
        /** @brief Cancels and joins every accepted child before releasing the scope. */
        ~TaskGroup();
        TaskGroup(const TaskGroup &) = delete;
        TaskGroup &operator=(const TaskGroup &) = delete;
        TaskGroup(TaskGroup &&) = delete;
        TaskGroup &operator=(TaskGroup &&) = delete;

        /**
         * @brief Admits one result-returning child job while the group is open.
         * @param descriptor Child submission metadata; its parent cancellation is replaced by the group token.
         * @param work Owned child callback.
         * @return Accepted child identifier or a typed admission failure.
         */
        [[nodiscard]] Result<JobId> Spawn(JobDescriptor descriptor, JobFunction work) const;
        /**
         * @brief Admits one context-aware child correlated to this group.
         * @param descriptor Child submission metadata; cancellation and task-group identity are replaced by the group.
         * @param work Owned child callback receiving immutable submission context.
         * @return Accepted child identifier or a typed admission failure.
         */
        [[nodiscard]] Result<JobId> SpawnContext(JobDescriptor descriptor, ContextJobFunction work) const;
        /** @brief Returns the stable process-local identity correlated to every accepted child. */
        [[nodiscard]] TaskGroupId Id() const noexcept;
        /** @brief Returns the immutable aggregate outcome after a completed join. @return Outcome, or no value while open/draining. */
        [[nodiscard]] std::optional<TaskGroupOutcome> Outcome() const;
        /** @brief Closes admission and requests cooperative cancellation for all accepted children. */
        void RequestCancel() const;
        /**
         * @brief Closes admission and joins every accepted child.
         * @return Success, the first child failure in spawn order, or the first cancellation if none failed.
         * Repeated calls return the same result.
         */
        [[nodiscard]] Result<void> Join() const;
        /**
         * @brief Closes admission and joins every accepted child under one finite deadline.
         * @param options Caller-affinity rule and maximum duration shared by the complete join.
         * @return Success, the first child failure, the first cancellation if none failed, or a typed wait-control error. A wait-control
         * error leaves the closed group retryable so its owner can cancel and drain it safely.
         */
        [[nodiscard]] Result<void> Join(const JoinOptions &options) const;

    private:
        struct State;

        struct ChildJoinOutcome {
            std::optional<Error> firstFailure;
            std::optional<Error> firstCancellation;
            std::optional<Error> interruption;
        };

        static void CancelChildren(const std::shared_ptr<State> &state);
        static ChildJoinOutcome WaitForChildren(const std::vector<std::shared_ptr<JobRecord>> &children, const JoinOptions *options);
        [[nodiscard]] Result<void> JoinImpl(const JoinOptions *options) const;
        std::shared_ptr<State> m_state;
    };
}  // namespace Horo
