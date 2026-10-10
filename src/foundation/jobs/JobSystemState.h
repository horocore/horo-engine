#pragma once

#include "Horo/Foundation/JobSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/** @file
 * @brief Target-private scheduler state shared by execution and bounded admission.
 * One State owns all queue/producer facts under mutex; callbacks and telemetry remain outside it.
 * This header is neither installed nor added to any public include boundary.
 */
namespace Horo {
    struct JobRecord;
    struct JobStoreState;

    struct SchedulerIdentity {
        std::uint64_t value{};
        [[nodiscard]] friend bool operator==(const SchedulerIdentity &, const SchedulerIdentity &) = default;
    };

    /** @brief Tracks callback/capture lifetime under mutex, including explicit inline wait execution.
     * Shutdown closes helper admission then waits for active callbacks; workers may finish draining afterward.
     * Records share this owner so durable handles cannot escape the rejection/join boundary.
     */
    struct JobExecutionState final {
        explicit JobExecutionState(const JobSystemConfig &config)
            : limits{std::max<std::size_t>(1, config.workerCount), config.ioWorkerCount} {}

        std::mutex mutex;
        std::condition_variable completed;
        bool helpersAllowed = true;
        std::size_t active{};
        const std::array<std::size_t, 2> limits;
        std::array<std::size_t, 2> occupied{};
    };

    struct JobSystem::State {
        /** @brief Copies immutable configuration and creates one execution/admission state owner. */
        explicit State(const JobSystemConfig &value);

        const JobSystemConfig config;
        SchedulerIdentity schedulerIdentity;
        std::mutex mutex;
        std::condition_variable workAvailable;
        bool accepting = true;
        bool stopping = false;
        JobId nextId = 1;
        // Admission/pop and all queue counts share mutex; callbacks and telemetry execute outside it.
        std::array<std::deque<std::shared_ptr<JobRecord>>, 3> queues;
        std::array<std::deque<std::shared_ptr<JobRecord>>, 3> ioQueues;
        std::array<std::size_t, 2> dispatchCursor{};
        std::shared_ptr<JobExecutionState> execution;
        std::shared_ptr<std::condition_variable> spaceAvailable = std::make_shared<std::condition_variable>();
        JobAdmissionSnapshot admission;
        std::array<std::deque<std::uint64_t>, 3> producerOrder;
        std::uint64_t nextProducer = 1;
        std::shared_ptr<JobStoreState> store;
        std::vector<std::thread> workers;  // NOSONAR(cpp:S6168) std::jthread not supported by AppleClang libc++ without experimental flags

        std::mutex shutdownMutex;

        /** @brief Enforces role, FIFO producer order and one finite deadline with scheduler mutex held on entry/return. */
        [[nodiscard]] Result<void> AwaitSpace(std::unique_lock<std::mutex> &lock, const JobDescriptor &descriptor, std::size_t priority,
                                              bool &overloaded, bool executionThread);

        /** @brief Admits and publishes one record under the scheduler lock, ensuring callback destruction occurs after unlocking. */
        [[nodiscard]] Result<JobHandle> Admit(const JobDescriptor &descriptor, ContextJobFunction work, std::size_t priority,
                                              bool &overloaded, ContextJobFunction &releasedWork);

        /** @brief Publishes initialized work atomically to retention and execution queues; rolls back insertion on allocation failure.
         * @pre The scheduler mutex is held, callback ownership is initialized, and the record has not been published.
         * @pre The caller retains the record until after releasing the scheduler lock, including exceptional exits.
         */
        void PublishRecord(const std::shared_ptr<JobRecord> &record, std::size_t priority, bool cancelled);

        /** @brief Validates immutable policy before any admission side effects. */
        [[nodiscard]] Result<void> ValidateAdmission(std::size_t priority, JobRequirement requirement, JobResource resource) const;

        /** @brief Records a rejected decision with scheduler mutex held. */
        void RecordRejection(std::size_t priority, const Error &error);

        /** @brief Checks whether this caller can safely join the bounded producer wait set. */
        [[nodiscard]] Result<void> ValidateBlockingAdmission(std::size_t priority, bool executionThread, JobResource resource) const;

        /** @brief Applies terminal decision precedence while retaining the producer's FIFO position. */
        [[nodiscard]] Result<void> WaitForAdmission(std::unique_lock<std::mutex> &lock, const JobDescriptor &descriptor,
                                                    std::size_t priority, std::uint64_t ticket);

        /** @brief Removes a bounded waiter and wakes its successor on every admission outcome while scheduler mutex is held. */
        struct ProducerLease final {
            ProducerLease(State &owner, const std::size_t queuePriority, const std::uint64_t producerTicket)
                : state(owner), priority(queuePriority), ticket(producerTicket) {}

            ProducerLease(const ProducerLease &) = delete;
            ProducerLease &operator=(const ProducerLease &) = delete;
            ProducerLease(ProducerLease &&) = delete;
            ProducerLease &operator=(ProducerLease &&) = delete;

            State &state;
            std::size_t priority;
            std::uint64_t ticket;

            ~ProducerLease() {
                std::erase(state.producerOrder[priority], ticket);
                --state.admission.waitingProducers;
                state.spaceAvailable->notify_all();
            }
        };

        [[nodiscard]] std::size_t QueuedCount() const noexcept {
            return queues[0].size() + queues[1].size() + queues[2].size() + ioQueues[0].size() + ioQueues[1].size() + ioQueues[2].size();
        }

        [[nodiscard]] auto &Queues(const JobResource resource) noexcept {
            return resource == JobResource::Io ? ioQueues : queues;
        }

        /** @brief Prunes terminal/claimed work while scheduler mutex is held. */
        void PruneQueues();

        [[nodiscard]] bool HasSpace(const std::size_t priority) const noexcept {
            const auto reserved = std::min(config.reservedInteractiveJobs, config.maxQueuedJobs);
            const auto limit = priority == 0 ? config.maxQueuedJobs : config.maxQueuedJobs - reserved;
            return QueuedCount() < limit && queues[priority].size() + ioQueues[priority].size() <
                                                config.priorityQueues[priority].capacity.value_or(config.maxQueuedJobs);
        }

        /** @brief Selects weighted FIFO work while scheduler mutex is held. */
        [[nodiscard]] std::shared_ptr<JobRecord> PopNext(JobResource resource);
    };

    namespace JobDetail {
        /** @brief Computes a saturating finite-wait deadline shared by admission and completion waits. */
        [[nodiscard]] std::chrono::steady_clock::time_point WaitDeadline(Duration timeout) noexcept;
    }  // namespace JobDetail
}  // namespace Horo
