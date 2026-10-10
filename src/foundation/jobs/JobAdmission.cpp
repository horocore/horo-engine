#include "../FoundationErrors.h"
#include "JobSystemState.h"

#include <algorithm>
#include <chrono>

namespace Horo {
    /** @copydoc JobDetail::WaitDeadline */
    std::chrono::steady_clock::time_point JobDetail::WaitDeadline(const Duration timeout) noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::nanoseconds(std::max<std::int64_t>(0, timeout.ToNanoseconds()));
        const auto available = std::chrono::steady_clock::time_point::max() - now;
        return remaining >= available ? std::chrono::steady_clock::time_point::max() : now + remaining;
    }

    /** @copydoc JobSystem::State::PopNext */
    std::shared_ptr<JobRecord> JobSystem::State::PopNext(const JobResource resource) {
        static constexpr std::array<std::size_t, 7> order{0, 0, 0, 0, 1, 1, 2};
        auto &cursor = dispatchCursor[static_cast<std::size_t>(resource)];
        auto &resourceQueues = Queues(resource);
        const auto initialCursor = cursor;
        do {
            const auto priority = order[cursor];
            cursor = (cursor + 1) % order.size();
            if (resourceQueues[priority].empty())
                continue;
            auto record = std::move(resourceQueues[priority].front());
            resourceQueues[priority].pop_front();
            spaceAvailable->notify_all();
            return record;
        } while (cursor != initialCursor);
        return {};
    }

    /** @copydoc JobProducerScope::JobProducerScope */
    JobProducerScope::JobProducerScope(const JobProducerRole role) noexcept : previousPermission_(BlockingPermission()) {
        BlockingPermission() = role == JobProducerRole::NonCritical && previousPermission_.value_or(true);
    }

    /** @copydoc JobProducerScope::~JobProducerScope */
    JobProducerScope::~JobProducerScope() {
        BlockingPermission() = previousPermission_;
    }

    /** @brief Encapsulates thread-owned host permission; no mutation or lifetime crosses producer threads. */
    std::optional<bool> &JobProducerScope::BlockingPermission() noexcept {
        static thread_local std::optional<bool> permission;
        return permission;
    }

    /** @copydoc JobSystem::State::AwaitSpace */
    Result<void> JobSystem::State::AwaitSpace(std::unique_lock<std::mutex> &lock, const JobDescriptor &descriptor,
                                              const std::size_t priority, bool &overloaded, const bool executionThread) {
        const auto &policy = config.priorityQueues[priority];
        PruneQueues();
        if (descriptor.parentCancellation.IsCancellationRequested() || (HasSpace(priority) && producerOrder[priority].empty()))
            return Result<void>::Success();
        overloaded = true;
        if (policy.overloadPolicy == JobOverloadPolicy::Shed && descriptor.requirement == JobRequirement::Optional)
            return Result<void>::Failure(MakeError(JobErrors::QueueShed, "Incoming optional job was shed at capacity."));
        if (policy.overloadPolicy != JobOverloadPolicy::Block)
            return Result<void>::Failure(MakeError(JobErrors::QueueFull, "Job queue is at capacity."));
        if (const auto validated = ValidateBlockingAdmission(priority, executionThread, descriptor.resource); validated.HasError())
            return validated;

        const auto cancellationWake = descriptor.parentCancellation.RegisterAdmissionWake(spaceAvailable);
        const auto ticket = nextProducer++;
        producerOrder[priority].push_back(ticket);
        ++admission.waitingProducers;
        const ProducerLease lease{*this, priority, ticket};
        return WaitForAdmission(lock, descriptor, priority, ticket);
    }

    /** @copydoc JobSystem::State::ValidateBlockingAdmission */
    Result<void> JobSystem::State::ValidateBlockingAdmission(const std::size_t priority, const bool executionThread,
                                                             const JobResource resource) const {
        if (!JobProducerScope::BlockingPermission().value_or(false) || executionThread)
            return Result<void>::Failure(MakeError(JobErrors::WaitForbidden, "This producer may not block on admission."));
        if ((resource == JobResource::Cpu ? config.workerCount : config.ioWorkerCount) == 0 || config.maxQueuedJobs == 0 ||
            config.priorityQueues[priority].capacity.value_or(config.maxQueuedJobs) == 0)
            return Result<void>::Failure(MakeError(JobErrors::WaitCapacityDeadlock, "Queue has no execution or admission capacity."));
        if (admission.waitingProducers >= config.maxWaitingProducers)
            return Result<void>::Failure(MakeError(JobErrors::QueueFull, "Bounded producer waiting capacity is exhausted."));
        return Result<void>::Success();
    }

    /** @copydoc JobSystem::State::WaitForAdmission */
    Result<void> JobSystem::State::WaitForAdmission(std::unique_lock<std::mutex> &lock, const JobDescriptor &descriptor,
                                                    const std::size_t priority, const std::uint64_t ticket) {
        const auto deadline = JobDetail::WaitDeadline(config.priorityQueues[priority].blockTimeout);
        for (;;) {
            // Fixed decision precedence at this synchronization point: shutdown, cancellation, deadline, then capacity.
            if (!accepting)
                return Result<void>::Failure(MakeError(JobErrors::Shutdown, "Job system stopped during admission wait."));
            if (descriptor.parentCancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(JobErrors::Cancelled, "Submission was cancelled before admission."));
            if (std::chrono::steady_clock::now() >= deadline)
                return Result<void>::Failure(MakeError(JobErrors::WaitTimedOut, "Queue admission deadline expired."));
            PruneQueues();
            if (producerOrder[priority].front() == ticket && HasSpace(priority))
                return Result<void>::Success();
            // Cancellation may notify without scheduler mutex. Bounded rechecks close the missed-notification race.
            spaceAvailable->wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
        }
    }

    /** @copydoc JobSystem::State::ValidateAdmission */
    Result<void> JobSystem::State::ValidateAdmission(const std::size_t priority, const JobRequirement requirement,
                                                     const JobResource resource) const {
        using enum JobOverloadPolicy;
        if (priority >= queues.size())
            return Result<void>::Failure(MakeError(JobErrors::InvalidSubmission, "Job priority is not recognized."));
        if (resource != JobResource::Cpu && resource != JobResource::Io)
            return Result<void>::Failure(MakeError(JobErrors::InvalidSubmission, "Job resource is not recognized."));
        if (resource == JobResource::Io && config.ioWorkerCount == 0)
            return Result<void>::Failure(MakeError(JobErrors::WaitCapacityDeadlock, "Host has no I/O execution capacity."));
        if (requirement != JobRequirement::Required && requirement != JobRequirement::Optional)
            return Result<void>::Failure(MakeError(JobErrors::InvalidSubmission, "Job requirement is not recognized."));
        const auto &queueConfig = config.priorityQueues[priority];
        if (queueConfig.overloadPolicy != Reject && queueConfig.overloadPolicy != Block && queueConfig.overloadPolicy != Shed)
            return Result<void>::Failure(MakeError(JobErrors::InvalidSubmission, "Queue overload policy is not recognized."));
        if (queueConfig.overloadPolicy == Block && queueConfig.blockTimeout.ToNanoseconds() <= 0)
            return Result<void>::Failure(MakeError(JobErrors::InvalidSubmission, "Blocking admission requires a positive timeout."));
        return Result<void>::Success();
    }

    /** @copydoc JobSystem::State::RecordRejection */
    void JobSystem::State::RecordRejection(const std::size_t priority, const Error &error) {
        ++admission.rejected[priority];
        if (error.code.Value() == JobErrors::QueueShed.code.Value())
            ++admission.shed[priority];
        if (error.code.Value() == JobErrors::WaitTimedOut.code.Value())
            ++admission.timedOut[priority];
    }

}  // namespace Horo
