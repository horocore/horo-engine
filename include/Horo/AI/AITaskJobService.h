#pragma once

/** @file AITaskJobService.h
 * @brief Explicit bounded Foundation-job integration for decision task continuations.
 */

#include "Horo/AI/AITaskContinuation.h"
#include "Horo/Foundation/JobSystem.h"

#include <utility>
#include <vector>

namespace Horo::AI {
    /** @brief Detached task work; implementations own immutable inputs and hold no mutable Scene capabilities. */
    class IAiTaskJob {
    public:
        virtual ~IAiTaskJob() = default;
        /** @brief Runs bounded cooperative computation on the Foundation worker pool.
         * @param operation Exact detached task identity; no live Scene or blackboard borrows.
         * @param cancellation Job cancellation ancestry; check at bounded intervals.
         * @return Success, original typed failure, or Foundation JobCancelled acknowledgement.
         */
        [[nodiscard]] virtual Result<void> Run(const AiTaskOperationContext &operation, const CancellationToken &cancellation) const = 0;
    };

    /**
     * @brief Copyable lease of one immutable work/image pair, destroyed in work-before-image order.
     * @details Capturing a pair allocates once at adapter composition/admission; copying and replacing leases
     * only retain the pair. Workers retain the same owned pair through callback destruction.
     */
    class AiTaskJobLease final {
    public:
        AiTaskJobLease() = default;
        /** @brief Captures owned work with the exact code and dependency lifetime pin.
         * @param imageLease Required pin covering callback code, work destruction and dependencies.
         * @param taskWork Owned immutable worker inputs and computation.
         * @throws std::bad_alloc When owned pair storage cannot be allocated.
         */
        AiTaskJobLease(std::shared_ptr<const void> imageLease, std::shared_ptr<const IAiTaskJob> taskWork);
        /** @brief Checks both required capabilities. @return True when work and its lifetime pin are present. */
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        friend class AiTaskJobService;
        struct State;
        /** @brief Invokes the pinned work, after service admission validated the pair.
         * @param operation Detached execution identity. @param cancellation Cooperative job token.
         * @return Original typed worker result.
         */
        [[nodiscard]] Result<void> Run(const AiTaskOperationContext &operation, const CancellationToken &cancellation) const;
        std::shared_ptr<const State> state_;
    };

    /**
     * @brief Single-owner bounded job tracker, composed explicitly with the process JobSystem.
     * @details Start/Cancel/Pump/Shutdown run serially on the decision owner. Workers capture only detached
     * values and an owned work/image lease, never this service. Pump converts scheduler terminal records into
     * canonical task candidates; tree evaluation remains the sole terminal authority. Cancelled running work
     * continues consuming capacity until terminal. Shutdown closes admission and cancels without frame waits.
     * Destruction may precede workers: their leases pin code and inputs through callback destruction. The host
     * must retain/drain the JobSystem and honor those image pins before unloading code or callback dependencies.
     */
    class AiTaskJobService final {
    public:
        /** @brief Factory-only construction authority; prevents bypassing capacity admission. */
        class ConstructionKey final {
            friend class AiTaskJobService;
            ConstructionKey() = default;
        };

        /** @brief Allocates admitted storage; only Create can supply the construction authority.
         * @param key Factory-owned construction authority.
         * @param jobs Borrowed process scheduler. @param capacity Validated capacity.
         */
        AiTaskJobService(ConstructionKey, JobSystem &jobs, std::size_t capacity);
        static constexpr std::size_t HardCapacity = 1'024;
        /** @brief Allocates finite tracking storage at composition.
         * @param jobs Borrowed process scheduler, outliving this service.
         * @param capacity Positive project-lowerable capacity, at most HardCapacity.
         * @return Owned service or typed capacity/storage failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<AiTaskJobService>> Create(JobSystem &jobs, std::size_t capacity = HardCapacity);
        /** @brief Admits one job for an exact current continuation.
         * @param continuation Instance-issued task generation and owner mailbox lease.
         * @param lease Owned work and exact image/callback lifetime pin.
         * @return Success or original typed admission failure, without an orphaned accepted job.
         */
        [[nodiscard]] Result<void> Start(AiTaskContinuation continuation, AiTaskJobLease lease);
        /** @brief Requests downstream cancellation, preserving already terminal scheduler results.
         * @param task Exact execution identity. @param reason Typed cancellation origin.
         * @return Success when absent/already cancelled, or typed invalid-input failure.
         */
        [[nodiscard]] Result<void> Cancel(TaskHandle task, AiTaskCancellationReason reason);
        /** @brief Processes at most capacity records once; contention retains candidates for the next owner phase. */
        void Pump();
        /** @brief Cancels all exact-agent records; running records retain capacity until terminal.
         * @param agent Exact agent generation. @param reason Valid cancellation origin. */
        void CancelAgent(AgentHandle agent, AiTaskCancellationReason reason) noexcept;
        /** @brief Counts retained work for one exact agent. @param agent Exact generation. @return Bounded retained count. */
        [[nodiscard]] std::size_t PendingCount(AgentHandle agent) const noexcept;
        /** @brief Closes admission and requests cancellation once; workers retain their own leases. */
        void Shutdown() noexcept;
        /** @brief Returns retained running or pending-publication count. @return Count bounded by admitted capacity. */
        [[nodiscard]] std::size_t PendingCount() const noexcept;
        /** @brief Closes admission; worker-owned pins survive until callbacks are destroyed. */
        ~AiTaskJobService();
        AiTaskJobService(const AiTaskJobService &) = delete;
        AiTaskJobService &operator=(const AiTaskJobService &) = delete;
        AiTaskJobService(AiTaskJobService &&) = delete;
        AiTaskJobService &operator=(AiTaskJobService &&) = delete;

    private:
        struct Entry;
        JobSystem &jobs_;
        std::vector<Entry> entries_;
        bool closed_{};
    };
}  // namespace Horo::AI
