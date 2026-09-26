#include "Horo/Foundation/JobSystem.h"

#include "../FoundationErrors.h"
#include "Horo/Foundation/Telemetry/Operation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Horo {
    struct JobStoreState;

    namespace {
        [[nodiscard]] Error MakeJobError(const ErrorCodeDescriptor &descriptor, const char *message) {
            return MakeError(descriptor, message);
        }

        [[nodiscard]] bool IsTerminal(const JobState state) noexcept {
            using enum JobState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        struct SchedulerIdentity {
            std::uint64_t value{};

            [[nodiscard]] friend bool operator==(const SchedulerIdentity &, const SchedulerIdentity &) = default;
        };

        [[nodiscard]] SchedulerIdentity NextSchedulerIdentity() noexcept {
            static std::atomic<std::uint64_t> next{1};
            return SchedulerIdentity{next.fetch_add(1)};
        }
    }  // namespace

    /** @copydoc JobCancelled */
    Result<void> JobCancelled(std::optional<Error> cause) {
        Error cancellation = MakeJobError(JobErrors::Cancelled, "Job acknowledged cancellation.");
        if (cause.has_value())
            cancellation = WithCause(std::move(cancellation), std::move(*cause));
        return Result<void>::Failure(std::move(cancellation));
    }

    /** @copydoc IsJobCancelled */
    bool IsJobCancelled(const Error &error) noexcept {
        return error.domain.Value() == JobErrors::Cancelled.domain.Value() && error.code.Value() == JobErrors::Cancelled.code.Value();
    }

    /** @brief Encapsulates the mutex shared by internal aggregate state without exposing the synchronization primitive as data. */
    class SynchronizedStateMutex {
    public:
        [[nodiscard]] std::mutex &Mutex() const noexcept {
            return mutex_;
        }

    private:
        mutable std::mutex mutex_;
    };

    struct JobRecord : private SynchronizedStateMutex {
        using SynchronizedStateMutex::Mutex;

        JobRecord(const JobId jobId, const JobDescriptor &descriptor, ContextJobFunction jobWork, const SchedulerIdentity scheduler,
                  std::weak_ptr<JobStoreState> owner)
            : id(jobId), cancellation(descriptor.parentCancellation), work(std::move(jobWork)), operationId(descriptor.operationId),
              taskGroupId(descriptor.taskGroupId), configuration(descriptor.configuration), schedulerIdentity(scheduler),
              store(std::move(owner)) {
            timing.submittedAt = std::chrono::steady_clock::now();
        }

        [[nodiscard]] static JobExecutionContext ExecutionContext(std::shared_ptr<JobRecord> record) {
            return JobExecutionContext(std::move(record));
        }

        JobId id;
        std::condition_variable completed;
        JobState state = JobState::Queued;
        JobProgress progress;
        JobTiming timing;
        std::optional<JobTerminalResult> terminalResult;
        CancellationSource cancellation;
        ContextJobFunction work;
        Telemetry::OperationContext operationContext = Telemetry::CaptureOperationContext();
        std::optional<OperationId> operationId;
        TaskGroupId taskGroupId;
        std::optional<ConfigurationSnapshotRef> configuration;
        SchedulerIdentity schedulerIdentity;
        std::weak_ptr<JobStoreState> store;
        std::thread::id submittingThread = std::this_thread::get_id();
    };

    struct JobStoreState final : private SynchronizedStateMutex {
        using SynchronizedStateMutex::Mutex;

        explicit JobStoreState(const std::size_t capacity) : terminalCapacity(capacity) {}

        const std::size_t terminalCapacity;
        std::unordered_map<JobId, std::shared_ptr<JobRecord>> records;
        std::deque<JobId> terminalOrder;
        std::uint64_t revision{};
        std::uint64_t droppedTerminalCount{};
    };

    struct JobSystem::State {
        explicit State(const JobSystemConfig value)
            : config(value), store(std::make_shared<JobStoreState>(value.maxRetainedTerminalJobs)) {}

        JobSystemConfig config;
        SchedulerIdentity schedulerIdentity = NextSchedulerIdentity();
        std::mutex mutex;
        std::condition_variable workAvailable;
        bool accepting = true;
        bool stopping = false;
        JobId nextId = 1;
        std::deque<std::shared_ptr<JobRecord>> queue;
        std::shared_ptr<JobStoreState> store;
        std::vector<std::thread> workers;  // NOSONAR(cpp:S6168) std::jthread not supported by AppleClang libc++ without experimental flags

        std::mutex shutdownMutex;
    };

    namespace {
        // Mutable scheduler identity is isolated per worker thread.
        thread_local std::optional<SchedulerIdentity> activeSchedulerIdentity;  // NOSONAR(cpp:S5421)

        struct JobExecutionFrame final {
            const JobRecord &record;
            std::optional<std::reference_wrapper<const JobExecutionFrame>> previous;
        };

        // Mutable execution stack is isolated per worker thread.
        thread_local std::optional<std::reference_wrapper<const JobExecutionFrame>> activeExecutionFrame;  // NOSONAR(cpp:S5421)

        /** @brief Tracks nested exact-record execution so synchronous wait cycles can be rejected. */
        class JobExecutionScope final {
        public:
            explicit JobExecutionScope(const JobRecord &record) noexcept : frame_{record, activeExecutionFrame} {
                activeExecutionFrame = std::cref(frame_);
            }

            ~JobExecutionScope() {
                activeExecutionFrame = frame_.previous;
            }

            JobExecutionScope(const JobExecutionScope &) = delete;
            JobExecutionScope &operator=(const JobExecutionScope &) = delete;

        private:
            JobExecutionFrame frame_;
        };

        [[nodiscard]] bool IsOnExecutionStack(const JobRecord &record) noexcept {
            for (auto frame = activeExecutionFrame; frame.has_value(); frame = frame->get().previous) {
                if (&frame->get().record == &record)
                    return true;
            }
            return false;
        }

        void RetainTerminalRecord(JobStoreState &store, const JobId id) {
            store.terminalOrder.push_back(id);
            while (store.terminalOrder.size() > store.terminalCapacity) {
                const JobId evicted = store.terminalOrder.front();
                store.terminalOrder.pop_front();
                store.records.erase(evicted);
                ++store.droppedTerminalCount;
            }
        }

        [[nodiscard]] JobSnapshot SnapshotRecord(const JobRecord &record) {
            std::lock_guard lock(record.Mutex());
            return JobSnapshot{.id = record.id,
                               .state = record.state,
                               .progress = record.progress,
                               .timing = record.timing,
                               .terminalResult = record.terminalResult,
                               .operationId = record.operationId,
                               .taskGroupId = record.taskGroupId,
                               .configurationRevision =
                                   record.configuration.has_value() ? std::optional{record.configuration->Revision()} : std::nullopt,
                               .error = record.terminalResult.has_value() ? record.terminalResult->error : std::nullopt};
        }

        [[nodiscard]] std::shared_ptr<JobRecord> FindRetainedRecord(const std::shared_ptr<JobStoreState> &store, const JobId id) {
            std::lock_guard lock(store->Mutex());
            const auto found = store->records.find(id);
            return found == store->records.end() ? nullptr : found->second;
        }

        [[nodiscard]] ContextJobFunction TransitionTerminalLocked(JobRecord &record, JobStoreState *store, const JobState state,
                                                                  std::optional<Error> error, const bool queuedOnly) {
            ContextJobFunction releasedWork;
            if (IsTerminal(record.state) || (queuedOnly && record.state != JobState::Queued))
                return releasedWork;
            record.state = state;
            record.terminalResult = JobTerminalResult{.state = state, .error = std::move(error)};
            record.timing.finishedAt = std::chrono::steady_clock::now();
            releasedWork.swap(record.work);
            record.completed.notify_all();
            if (store != nullptr) {
                ++store->revision;
                RetainTerminalRecord(*store, record.id);
            }
            return releasedWork;
        }

        [[nodiscard]] ContextJobFunction TransitionTerminal(const std::shared_ptr<JobRecord> &record, const JobState state,
                                                            std::optional<Error> error, const bool queuedOnly) {
            if (const std::shared_ptr store = record->store.lock()) {
                std::scoped_lock locks(store->Mutex(), record->Mutex());
                return TransitionTerminalLocked(*record, store.get(), state, std::move(error), queuedOnly);
            }
            std::lock_guard recordLock(record->Mutex());
            return TransitionTerminalLocked(*record, nullptr, state, std::move(error), queuedOnly);
        }

        [[nodiscard]] ContextJobFunction SetTerminalState(const std::shared_ptr<JobRecord> &record, const JobState state,
                                                          std::optional<Error> error = std::nullopt) {
            return TransitionTerminal(record, state, std::move(error), false);
        }

        [[nodiscard]] ContextJobFunction CancelQueuedRecord(const std::shared_ptr<JobRecord> &record, const char *message) {
            return TransitionTerminal(record, JobState::Cancelled, MakeJobError(JobErrors::Cancelled, message), true);
        }

        [[nodiscard]] ContextJobFunction RequestCancelRecord(const std::shared_ptr<JobRecord> &record, const char *message) {
            const auto requestLocked = [&record, message](JobStoreState *store) {
                if (IsTerminal(record->state))
                    return ContextJobFunction{};
                record->cancellation.RequestCancellation();
                if (record->state == JobState::Queued)
                    return TransitionTerminalLocked(*record, store, JobState::Cancelled, MakeJobError(JobErrors::Cancelled, message), true);
                return ContextJobFunction{};
            };
            if (const std::shared_ptr store = record->store.lock()) {
                std::scoped_lock locks(store->Mutex(), record->Mutex());
                return requestLocked(store.get());
            }
            std::lock_guard recordLock(record->Mutex());
            return requestLocked(nullptr);
        }

        void ExecuteJobRecord(const std::shared_ptr<JobRecord> &record) {
            const Telemetry::ScopedOperationContext operationContext{record->operationContext};
            const JobExecutionScope executionScope{*record};
            ContextJobFunction work;
            {
                std::lock_guard lock(record->Mutex());
                work.swap(record->work);
            }
            try {
                const JobExecutionContext context = JobRecord::ExecutionContext(record);
                Result<void> outcome = work(context);
                if (outcome.HasError()) {
                    const bool cancelled = IsJobCancelled(outcome.ErrorValue());
                    static_cast<void>(SetTerminalState(record, cancelled ? JobState::Cancelled : JobState::Failed, outcome.ErrorValue()));
                } else {
                    static_cast<void>(SetTerminalState(record, JobState::Succeeded));
                }
            } catch (const std::runtime_error &exception) {  // NOSONAR(cpp:S1181)
                static_cast<void>(SetTerminalState(record, JobState::Failed, MakeJobError(JobErrors::Failed, exception.what())));
            } catch (const std::logic_error &exception) {  // NOSONAR(cpp:S1181)
                static_cast<void>(SetTerminalState(record, JobState::Failed, MakeJobError(JobErrors::Failed, exception.what())));
            } catch (const std::bad_alloc &exception) {  // NOSONAR(cpp:S1181)
                static_cast<void>(SetTerminalState(record, JobState::Failed, MakeJobError(JobErrors::Failed, exception.what())));
            } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181)
                static_cast<void>(SetTerminalState(record, JobState::Failed, MakeJobError(JobErrors::Failed, exception.what())));
            } catch (...) {  // NOSONAR(cpp:S1181)
                static_cast<void>(SetTerminalState(record, JobState::Failed,
                                                   MakeJobError(JobErrors::Failed, "Job callback threw an unknown exception.")));
            }
            work = {};
        }

        [[nodiscard]] bool ClaimRecordLocked(JobRecord &record, JobStoreState *store, ContextJobFunction &releasedWork) {
            if (record.state != JobState::Queued)
                return false;
            if (record.cancellation.Token().IsCancellationRequested()) {
                releasedWork = TransitionTerminalLocked(record, store, JobState::Cancelled,
                                                        MakeJobError(JobErrors::Cancelled, "Job was cancelled before execution."), true);
                return false;
            }
            record.state = JobState::Running;
            record.timing.startedAt = std::chrono::steady_clock::now();
            if (store != nullptr)
                ++store->revision;
            return true;
        }

        [[nodiscard]] bool TryClaimJobRecord(const std::shared_ptr<JobRecord> &record) {
            ContextJobFunction releasedWork;
            bool claimed;
            if (const std::shared_ptr store = record->store.lock()) {
                std::scoped_lock locks(store->Mutex(), record->Mutex());
                claimed = ClaimRecordLocked(*record, store.get(), releasedWork);
            } else {
                std::lock_guard recordLock(record->Mutex());
                claimed = ClaimRecordLocked(*record, nullptr, releasedWork);
            }
            return claimed;
        }

        [[nodiscard]] Result<void> ValidateBoundedWait(const JobRecord &record, const WaitPolicy policy) {
            using enum WaitPolicy;
            if (IsOnExecutionStack(record))
                return Result<void>::Failure(
                    MakeJobError(JobErrors::WaitCapacityDeadlock, "A synchronous wait would close a re-entrant job execution cycle."));
            switch (policy) {
                case MainThreadPumpAllowed:
                    return Result<void>::Success();
                case OwnerThreadBlockAllowed:
                    if (record.submittingThread == std::this_thread::get_id())
                        return Result<void>::Success();
                    return Result<void>::Failure(
                        MakeJobError(JobErrors::WaitForbidden, "OwnerThreadBlockAllowed requires the submitting owner thread."));
                case WorkerOnly:
                    if (activeSchedulerIdentity.has_value() && *activeSchedulerIdentity == record.schedulerIdentity)
                        return Result<void>::Success();
                    return Result<void>::Failure(
                        MakeJobError(JobErrors::WaitForbidden, "WorkerOnly requires a worker executing on the owning job system."));
                case ForbiddenOnOwnerThread:
                    if (record.submittingThread != std::this_thread::get_id())
                        return Result<void>::Success();
                    return Result<void>::Failure(
                        MakeJobError(JobErrors::WaitForbidden, "The submitting owner thread is forbidden from waiting for this job."));
            }
            return Result<void>::Failure(MakeJobError(JobErrors::WaitForbidden, "The bounded wait policy is not recognized."));
        }

        [[nodiscard]] Result<void> TerminalResult(const JobRecord &record) {
            if (record.state == JobState::Succeeded)
                return Result<void>::Success();
            if (record.terminalResult.has_value() && record.terminalResult->error.has_value())
                return Result<void>::Failure(*record.terminalResult->error);
            return Result<void>::Failure(MakeJobError(JobErrors::Failed, "Terminal job state did not retain its required error payload."));
        }

        [[nodiscard]] std::chrono::steady_clock::time_point WaitDeadline(const Duration timeout) noexcept {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = std::chrono::nanoseconds(std::max<std::int64_t>(0, timeout.ToNanoseconds()));
            const auto available = std::chrono::steady_clock::time_point::max() - now;
            return remaining >= available ? std::chrono::steady_clock::time_point::max() : now + remaining;
        }

        [[nodiscard]] Result<void> WaitUntil(const std::shared_ptr<JobRecord> &record, const std::chrono::steady_clock::time_point deadline,
                                             const WaitPolicy policy) {
            if (const Result<void> validated = ValidateBoundedWait(*record, policy); validated.HasError())
                return validated;

            if ((policy == WaitPolicy::MainThreadPumpAllowed || policy == WaitPolicy::WorkerOnly) && TryClaimJobRecord(record))
                ExecuteJobRecord(record);

            if (std::unique_lock lock(record->Mutex()); !record->completed.wait_until(lock, deadline, [record] {
                return IsTerminal(record->state);
            }))
                return Result<void>::Failure(MakeJobError(JobErrors::WaitTimedOut, "The job did not complete before its wait deadline."));
            return TerminalResult(*record);
        }

        [[nodiscard]] bool IsWaitControlError(const Error &error) {
            static const std::array<const ErrorCodeDescriptor *, 3> controlErrors{
                &JobErrors::WaitForbidden,
                &JobErrors::WaitTimedOut,
                &JobErrors::WaitCapacityDeadlock,
            };
            return std::ranges::any_of(controlErrors, [&error](const ErrorCodeDescriptor *descriptor) {
                return error.domain.Value() == descriptor->domain.Value() && error.code.Value() == descriptor->code.Value();
            });
        }

        [[nodiscard]] Result<void> ResultFromError(const std::optional<Error> &error) {
            return error.has_value() ? Result<void>::Failure(*error) : Result<void>::Success();
        }
    }  // namespace

    void JobSystem::RunWorker(const std::shared_ptr<State> &state) {
        for (;;) {
            std::shared_ptr<JobRecord> record;
            {
                std::unique_lock lock(state->mutex);
                state->workAvailable.wait(lock, [&state] {
                    return state->stopping || !state->queue.empty();
                });
                if (state->queue.empty()) {
                    if (state->stopping)
                        return;
                    continue;
                }
                record = std::move(state->queue.front());
                state->queue.pop_front();
            }

            if (!TryClaimJobRecord(record))
                continue;

            ExecuteJobRecord(record);
        }
    }

    JobSystem::JobSystem(const JobSystemConfig config) : m_state(std::make_shared<State>(config)) {
        for (std::size_t index = 0; index < config.workerCount; ++index)
            m_state->workers.emplace_back([state = m_state] {
                activeSchedulerIdentity = state->schedulerIdentity;
                RunWorker(state);
                activeSchedulerIdentity.reset();
            });
    }

    JobSystem::~JobSystem() {
        try {
            Shutdown(ShutdownPolicy::Cancel);
        } catch (...) {  // NOSONAR(cpp:S2486) A destructor cannot report or propagate cleanup failures.
            // Destructors must swallow any unexpected exceptions per noexcept contract.
        }
    }

    Result<JobHandle> JobSystem::Submit(JobDescriptor descriptor, std::function<void(const CancellationToken &)> work) const {
        return SubmitResult(std::move(descriptor), [work = std::move(work)](const CancellationToken &cancellation) {
            work(cancellation);
            return Result<void>::Success();
        });
    }

    Result<JobHandle> JobSystem::SubmitResult(JobDescriptor descriptor, JobFunction work) const {
        return SubmitContext(std::move(descriptor), [work = std::move(work)](const JobExecutionContext &context) {
            return work(context.Cancellation());
        });
    }

    /** @copydoc JobSystem::SubmitContext */
    Result<JobHandle> JobSystem::SubmitContext(JobDescriptor descriptor, ContextJobFunction work) const {
        std::shared_ptr<JobRecord> record;
        ContextJobFunction releasedWork;
        {
            std::lock_guard lock(m_state->mutex);
            if (!m_state->accepting)
                return Result<JobHandle>::Failure(MakeJobError(JobErrors::Shutdown, "Job system is no longer accepting work."));
            const bool cancelledBeforeAdmission = descriptor.parentCancellation.IsCancellationRequested();
            if (!cancelledBeforeAdmission && m_state->queue.size() >= m_state->config.maxQueuedJobs)
                std::erase_if(m_state->queue, [](const std::shared_ptr<JobRecord> &queued) {
                    std::lock_guard recordLock(queued->Mutex());
                    return queued->state != JobState::Queued;
                });
            if (!cancelledBeforeAdmission && m_state->queue.size() >= m_state->config.maxQueuedJobs)
                return Result<JobHandle>::Failure(MakeJobError(JobErrors::QueueFull, "Job queue is at capacity."));

            record =
                std::make_shared<JobRecord>(m_state->nextId++, descriptor, std::move(work), m_state->schedulerIdentity, m_state->store);
            {
                std::lock_guard storeLock(m_state->store->Mutex());
                m_state->store->records.try_emplace(record->id, record);
                ++m_state->store->revision;
            }
            if (record->cancellation.Token().IsCancellationRequested())
                releasedWork = CancelQueuedRecord(record, "Job was cancelled before execution.");
            else {
                m_state->queue.push_back(record);
                m_state->workAvailable.notify_one();
            }
        }
        releasedWork = {};
        return Result<JobHandle>::Success(JobHandle(std::move(record)));
    }

    Result<void> JobSystem::RequestCancel(const JobId id) const {
        const std::shared_ptr record = FindRetainedRecord(m_state->store, id);
        if (!record)
            return Result<void>::Failure(MakeJobError(JobErrors::NotFound, "Job identifier is not known by this job system."));
        ContextJobFunction releasedWork = RequestCancelRecord(record, "Job was cancelled before execution.");
        return Result<void>::Success();
    }

    JobSnapshot JobSystem::Query(const JobId id) const {
        return Find(id).value_or(JobSnapshot{.id = id});
    }

    /** @copydoc JobSystem::Find */
    std::optional<JobSnapshot> JobSystem::Find(const JobId id) const {
        const std::shared_ptr record = FindRetainedRecord(m_state->store, id);
        return record ? std::optional{SnapshotRecord(*record)} : std::nullopt;
    }

    /** @copydoc JobSystem::SnapshotIfChanged */
    std::optional<JobStoreSnapshot> JobSystem::SnapshotIfChanged(const std::uint64_t knownRevision) const {
        JobStoreSnapshot snapshot;
        {
            std::lock_guard lock(m_state->store->Mutex());
            if (knownRevision == m_state->store->revision)
                return std::nullopt;
            snapshot.revision = m_state->store->revision;
            snapshot.terminalCapacity = m_state->store->terminalCapacity;
            snapshot.droppedTerminalCount = m_state->store->droppedTerminalCount;
            snapshot.jobs.reserve(m_state->store->records.size());
            for (const auto &[id, record] : m_state->store->records) {
                (void)id;
                snapshot.jobs.push_back(SnapshotRecord(*record));
            }
        }
        std::ranges::sort(snapshot.jobs, {}, &JobSnapshot::id);
        return snapshot;
    }

    /** @copydoc JobSystem::WorkerCount */
    std::size_t JobSystem::WorkerCount() const noexcept {
        return m_state->config.workerCount;
    }

    void JobSystem::Shutdown(const ShutdownPolicy policy) const {
        std::vector<ContextJobFunction> releasedWork;
        {
            std::lock_guard shutdownLock(m_state->shutdownMutex);
            {
                // Queue admission/pop is serialized first; record completion never acquires the scheduler mutex.
                std::lock_guard lock(m_state->mutex);
                m_state->accepting = false;
                m_state->stopping = true;
                if (policy == ShutdownPolicy::Cancel) {
                    std::vector<std::shared_ptr<JobRecord>> records;
                    {
                        std::lock_guard storeLock(m_state->store->Mutex());
                        records.reserve(m_state->store->records.size());
                        for (const auto &[id, record] : m_state->store->records) {
                            (void)id;
                            records.push_back(record);
                        }
                    }
                    releasedWork.reserve(records.size());
                    for (const auto &record : records)
                        releasedWork.push_back(RequestCancelRecord(record, "Job was cancelled during shutdown."));
                    m_state->queue.clear();
                }
            }
            m_state->workAvailable.notify_all();
            std::ranges::for_each(m_state->workers, [](std::thread &worker) {  // NOSONAR(cpp:S6168) std::jthread not supported by
                                                                               // AppleClang libc++ without experimental flags
                if (worker.joinable()) {
                    worker.join();
                }
            });
        }
        releasedWork.clear();
    }

    Result<void> JobHandle::Wait() const {
        if (!m_record)
            return Result<void>::Failure(MakeJobError(JobErrors::InvalidHandle, "Cannot wait on an invalid job handle."));

        const auto record = m_record;
        std::unique_lock lock(record->Mutex());
        record->completed.wait(lock, [record] {
            return IsTerminal(record->state);
        });
        return TerminalResult(*record);
    }

    /** @copydoc JobHandle::Wait(const JoinOptions &) */
    Result<void> JobHandle::Wait(const JoinOptions &options) const {
        if (!m_record)
            return Result<void>::Failure(MakeJobError(JobErrors::InvalidHandle, "Cannot wait on an invalid job handle."));
        return WaitUntil(m_record, WaitDeadline(options.timeout), options.waitPolicy);
    }

    JobId JobHandle::Id() const noexcept {
        return m_record ? m_record->id : 0;
    }

    /** @copydoc JobHandle::Snapshot */
    std::optional<JobSnapshot> JobHandle::Snapshot() const {
        return m_record ? std::optional{SnapshotRecord(*m_record)} : std::nullopt;
    }

    /** @copydoc JobHandle::RequestCancel */
    Result<void> JobHandle::RequestCancel() const {
        if (!m_record)
            return Result<void>::Failure(MakeJobError(JobErrors::InvalidHandle, "Cannot cancel through an invalid job handle."));
        ContextJobFunction releasedWork = RequestCancelRecord(m_record, "Job was cancelled before execution.");
        return Result<void>::Success();
    }

    JobExecutionContext::JobExecutionContext(std::shared_ptr<JobRecord> record)
        : record_(std::move(record)), cancellation_(record_->cancellation.Token()) {}

    /** @copydoc JobExecutionContext::Cancellation */
    const CancellationToken &JobExecutionContext::Cancellation() const noexcept {
        return cancellation_;
    }

    /** @copydoc JobExecutionContext::Configuration */
    const std::optional<ConfigurationSnapshotRef> &JobExecutionContext::Configuration() const noexcept {
        return record_->configuration;
    }

    /** @copydoc JobExecutionContext::Operation */
    std::optional<OperationId> JobExecutionContext::Operation() const noexcept {
        return record_->operationId;
    }

    /** @copydoc JobExecutionContext::Group */
    TaskGroupId JobExecutionContext::Group() const noexcept {
        return record_->taskGroupId;
    }

    JobProgressPhase::JobProgressPhase(const std::string_view value) noexcept : size_(static_cast<std::uint8_t>(value.size())) {
        std::ranges::copy(value, characters_.begin());
    }

    /** @copydoc JobExecutionContext::UpdateProgress */
    Result<void> JobExecutionContext::UpdateProgress(const std::string_view phase, const std::optional<float> value) const {
        if (phase.empty() || phase.size() > JobProgressPhase::MaximumBytes ||
            (value.has_value() && (!std::isfinite(*value) || *value < 0.0F || *value > 1.0F)))
            return Result<void>::Failure(MakeJobError(JobErrors::InvalidProgress, "Job progress phase or normalized value is invalid."));

        const auto updateLocked = [this, phase, value](JobStoreState *store) {
            if (IsTerminal(record_->state))
                return Result<void>::Failure(MakeJobError(JobErrors::TerminalImmutable, "Terminal job progress cannot be changed."));
            if (const bool samePhase = record_->progress.phase.View() == phase;
                samePhase && record_->progress.value.has_value() && (!value.has_value() || *value < *record_->progress.value))
                return Result<void>::Failure(MakeJobError(JobErrors::ProgressRegressed, "Job progress cannot decrease within one phase."));
            record_->progress = JobProgress{.phase = JobProgressPhase{phase}, .value = value};
            if (store != nullptr)
                ++store->revision;
            return Result<void>::Success();
        };
        if (const std::shared_ptr store = record_->store.lock()) {
            std::scoped_lock locks(store->Mutex(), record_->Mutex());
            return updateLocked(store.get());
        }
        std::lock_guard recordLock(record_->Mutex());
        return updateLocked(nullptr);
    }

    struct TaskGroup::State {
        State(JobSystem &jobSystem, const TaskGroupFailurePolicy failurePolicy, const CancellationToken &parentCancellation)
            : jobs(jobSystem), policy(failurePolicy), cancellation(parentCancellation) {}

        [[nodiscard]] static TaskGroupId NextTaskGroupIdentity() noexcept {
            static std::atomic<std::uint64_t> next{1};
            return TaskGroupId{next.fetch_add(1)};
        }

        JobSystem &jobs;
        TaskGroupFailurePolicy policy;
        CancellationSource cancellation;
        TaskGroupId id = NextTaskGroupIdentity();
        std::mutex mutex;
        std::mutex joinMutex;
        bool accepting = true;
        bool joined = false;
        std::optional<Error> joinError;
        std::optional<TaskGroupOutcome> outcome;
        std::vector<JobHandle> children;
    };

    void TaskGroup::CancelChildren(const std::shared_ptr<State> &state) {
        std::vector<JobId> childIds;
        {
            std::lock_guard lock(state->mutex);
            state->accepting = false;
            state->cancellation.RequestCancellation();
            childIds.reserve(state->children.size());
            for (const auto &child : state->children)
                childIds.push_back(child.Id());
        }
        for (const JobId childId : childIds)
            static_cast<void>(state->jobs.RequestCancel(childId));
    }

    TaskGroup::TaskGroup(JobSystem &jobs, const TaskGroupFailurePolicy policy, const CancellationToken parentCancellation)
        : m_state(std::make_shared<State>(jobs, policy, parentCancellation)) {}

    TaskGroup::~TaskGroup() {
        try {
            RequestCancel();
            static_cast<void>(Join());
        } catch (...) {  // NOSONAR(cpp:S2486) A destructor cannot report or propagate cleanup failures.
            // Destructors must swallow any unexpected exceptions per noexcept contract
        }
    }

    Result<JobId> TaskGroup::Spawn(JobDescriptor descriptor, JobFunction work) const {
        return SpawnContext(std::move(descriptor), [work = std::move(work)](const JobExecutionContext &context) {
            return work(context.Cancellation());
        });
    }

    /** @copydoc TaskGroup::SpawnContext */
    Result<JobId> TaskGroup::SpawnContext(JobDescriptor descriptor, ContextJobFunction work) const {
        // Keep the callback alive until after the group lock is released: immediate cancellation
        // or admission rejection may otherwise destroy a capture that reenters this group.
        const auto ownedWork = std::make_shared<ContextJobFunction>(std::move(work));
        std::lock_guard lock(m_state->mutex);
        if (!m_state->accepting)
            return Result<JobId>::Failure(MakeJobError(JobErrors::TaskGroupClosed, "Task group admission is closed."));

        descriptor.parentCancellation = m_state->cancellation.Token();
        descriptor.taskGroupId = m_state->id;
        const std::weak_ptr weakState = m_state;
        Result<JobHandle> submitted =
            m_state->jobs.SubmitContext(std::move(descriptor), [weakState, ownedWork](const JobExecutionContext &context) {
            if (context.Cancellation().IsCancellationRequested())
                return Result<void>::Failure(MakeJobError(JobErrors::Cancelled, "Task group child was cancelled before execution."));
            const auto failFast = [&weakState] {
                if (const auto state = weakState.lock(); state && state->policy == TaskGroupFailurePolicy::FailFast)
                    CancelChildren(state);
            };
            Result<void> outcome = [&] {
                try {
                    return (*ownedWork)(context);
                } catch (...) {
                    failFast();
                    throw;
                }
            }();
            if (outcome.HasError() && !IsJobCancelled(outcome.ErrorValue()))
                failFast();
            return outcome;
        });
        if (submitted.HasError())
            return Result<JobId>::Failure(submitted.ErrorValue());

        JobHandle child = std::move(submitted).Value();
        const JobId childId = child.Id();
        m_state->children.push_back(std::move(child));
        return Result<JobId>::Success(childId);
    }

    /** @copydoc TaskGroup::Id */
    TaskGroupId TaskGroup::Id() const noexcept {
        return m_state->id;
    }

    /** @copydoc TaskGroup::Outcome */
    std::optional<TaskGroupOutcome> TaskGroup::Outcome() const {
        std::lock_guard lock(m_state->mutex);
        return m_state->outcome;
    }

    void TaskGroup::RequestCancel() const {
        CancelChildren(m_state);
    }

    Result<void> TaskGroup::Join() const {
        return JoinImpl(nullptr);
    }

    /** @copydoc TaskGroup::Join(const JoinOptions &) */
    Result<void> TaskGroup::Join(const JoinOptions &options) const {
        return JoinImpl(&options);
    }

    TaskGroup::ChildJoinOutcome TaskGroup::WaitForChildren(const std::vector<std::shared_ptr<JobRecord>> &children,
                                                           const JoinOptions *options) {
        const auto deadline = options == nullptr ? std::chrono::steady_clock::time_point{} : WaitDeadline(options->timeout);
        ChildJoinOutcome outcome;
        for (const auto &child : children) {
            const Result<void> waited = options == nullptr ? JobHandle{child}.Wait() : WaitUntil(child, deadline, options->waitPolicy);
            if (options != nullptr && waited.HasError() && IsWaitControlError(waited.ErrorValue())) {
                outcome.interruption = waited.ErrorValue();
                return outcome;
            }
            if (!waited.HasError())
                continue;
            if (IsJobCancelled(waited.ErrorValue())) {
                if (!outcome.firstCancellation.has_value())
                    outcome.firstCancellation = waited.ErrorValue();
            } else if (!outcome.firstFailure.has_value()) {
                outcome.firstFailure = waited.ErrorValue();
            }
        }
        return outcome;
    }

    Result<void> TaskGroup::JoinImpl(const JoinOptions *options) const {
        std::lock_guard joinLock(m_state->joinMutex);
        if (m_state->cancellation.Token().IsCancellationRequested())
            CancelChildren(m_state);

        std::vector<std::shared_ptr<JobRecord>> children;
        {
            std::lock_guard lock(m_state->mutex);
            if (m_state->joined)
                return ResultFromError(m_state->joinError);
            m_state->accepting = false;
            children.reserve(m_state->children.size());
            for (const auto &child : m_state->children)
                children.push_back(child.m_record);
        }

        const ChildJoinOutcome outcome = WaitForChildren(children, options);
        if (outcome.interruption.has_value())
            return Result<void>::Failure(*outcome.interruption);
        const std::optional<Error> &error = outcome.firstFailure.has_value() ? outcome.firstFailure : outcome.firstCancellation;
        {
            std::lock_guard lock(m_state->mutex);
            m_state->joined = true;
            m_state->joinError = error;
            using enum TaskGroupOutcome;
            m_state->outcome = Completed;
            if (outcome.firstFailure.has_value())
                m_state->outcome = Failed;
            else if (outcome.firstCancellation.has_value())
                m_state->outcome = Cancelled;
        }
        return ResultFromError(error);
    }
}  // namespace Horo
