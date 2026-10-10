#pragma once

/** @file NavigationCoordinator.h
 * @brief Bounded best-effort path batching and owner-phase result publication on the process JobSystem.
 */

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Navigation/NavigationPathPolicy.h"
#include "Horo/Navigation/NavigationRuntimeQueues.h"

#include <memory>
#include <optional>
#include <span>

namespace Horo::Navigation {
    /** @brief Logical caller identity and incarnation; replacement agents cannot consume old results. */
    struct NavigationPathCaller final {
        NavigationDynamicOwnerId owner;
        NavigationDynamicOwnerGeneration generation;
        constexpr auto operator<=>(const NavigationPathCaller &) const noexcept = default;
    };

    /** @brief Preparation bounds; admission and partition pressure retain pending work without blocking. */
    struct NavigationPathBatchLimits final {
        std::uint32_t requestSlots{64};                 /**< Power of two in [2, 4096], including unconsumed terminal results. */
        std::uint32_t maximumJobs{4};                   /**< Maximum concurrent Foundation partitions. */
        std::uint32_t requestsPerJob{8};                /**< Maximum serial queries in one compatible partition. */
        std::uint32_t requestsPerTick{16};              /**< Maximum dispatched queries per simulation tick. */
        std::uint32_t requestsPerCaller{1};             /**< Per-tick quota shared by all incarnations of one owner. */
        std::uint32_t maximumPendingPerCaller{16};      /**< Result-slot quota; must be less than requestSlots. */
        std::uint64_t nodeExpansionsPerTick{4096};      /**< Conservative sum of admitted node ceilings, shared across partitions. */
        std::uint64_t nodeExpansionsPerCaller{512};     /**< One caller cannot consume the whole configured node budget. */
        std::uint64_t priorityAgingTicks{30};           /**< Oldest requests outrank fresh priority after this delay. */
        std::size_t maximumOwnedBytes{4 * 1024 * 1024}; /**< Prepared inline storage ceiling; provider output uses admitted limits. */
    };

    /** @brief Owned admission input captured from one committed world root. */
    struct NavigationPathSubmission final {
        NavigationPathCaller caller;
        NavigationPathRequest request;
        NavigationOutcomeProvenance source; /**< Captured combined-root revisions, with whole-topology currentness. */
        std::uint64_t capabilityRevision{};
        std::uint64_t targetTick{};   /**< Earliest owner-phase publication tick. */
        std::uint64_t deadlineTick{}; /**< Inclusive final publication tick; must follow admission. */
        JobPriority priority{JobPriority::Normal};
        CancellationToken cancellation;
        std::optional<ConfigurationSnapshotRef> configuration;
        Telemetry::OperationContext operation;
    };

    /** @brief One owned terminal result; a partial corridor retains the provider's exact status and stop evidence. */
    struct NavigationPathCompletion final {
        NavRequestHandle handle;
        NavigationPathCaller caller;
        std::uint64_t acceptedSequence{};
        JobId job{}; /**< Zero when cancellation/deadline prevented dispatch. */
        NavigationOutcomeProvenance source;
        NavigationWorldActivationDescriptor activation;
        Result<NavigationPath> result;
    };

    /** @brief Fresh owner-phase authority from one committed root; spans are borrowed only during Commit. */
    struct NavigationPathPublication final {
        NavigationWorldActivationDescriptor activation; /**< All-zero means no active world after unload/shutdown. */
        NavigationOutcomeProvenance source;
        std::span<const NavigationPathCaller> callers;
        std::uint64_t tick{};
    };

    /**
     * @brief Owner-thread admission, bounded batching, cancellation and stable terminal publication.
     * @details The host injects its process JobSystem and calls Dispatch/Commit at fixed-tick boundaries. Submit never
     * invokes a provider or completes inline. Compatible immutable world leases/revisions share bounded partitions;
     * each invocation uses provider-owned exclusive scratch. Rotating caller service precedes per-caller deadline,
     * priority and aging selection. Worker arrival cannot bypass earlier eligible admission order or caller authority.
     * Prepared storage is bounded; Foundation callback/record and cancellation allocations are bounded by accepted
     * requests/jobs. This is best-effort execution: deterministic fixed-tick kernels remain a distinct host contract.
     * All methods except worker execution are owner-thread only. Destruction closes admission and signals cancellation
     * without joining; jobs retain only owned state/leases. The host must drain/shutdown JobSystem before unloading
     * provider code. No live Scene, gameplay callback, TaskGroup destructor wait, or new worker pool is retained.
     */
    class NavigationCoordinator final {
    public:
        /** @brief Prepare storage outside fixed-tick work.
         * @param jobs Process scheduler, configured with nonblocking CPU priority queues, outliving owner calls.
         * @param limits Finite request/partition/work/storage ceilings.
         * @return Prepared coordinator or typed descriptor/capacity failure.
         */
        [[nodiscard]] static Result<NavigationCoordinator> Create(JobSystem &jobs, NavigationPathBatchLimits limits);
        NavigationCoordinator(const NavigationCoordinator &) = delete;
        NavigationCoordinator &operator=(const NavigationCoordinator &) = delete;
        /** @brief Transfer quiescent owner authority. @param other Source becomes inert. */
        NavigationCoordinator(NavigationCoordinator &&other) noexcept;
        NavigationCoordinator &operator=(NavigationCoordinator &&) = delete;
        /** @brief Signal cancellation and release owner pins without waiting for workers. */
        ~NavigationCoordinator();

        /** @brief Admit an owned request without execution or publication.
         * @param world Owner-thread lifecycle issuing the exact provider lease.
         * @param submission Immutable caller/request/revision/context values.
         * @param tick Monotonic admission tick.
         * @return Generation-safe handle or typed error; rejection creates no accepted request/job.
         */
        [[nodiscard]] Result<NavRequestHandle> Submit(NavigationWorldLifecycle &world, NavigationPathSubmission submission,
                                                      std::uint64_t tick);
        /** @brief Cancel one generation before publication. @param handle Exact admitted identity.
         * @return False for invalid/consumed/terminal identity; true for a pending cancellation request.
         */
        [[nodiscard]] bool Cancel(NavRequestHandle handle) noexcept;
        /** @brief Dispatch one bounded tick slice without waiting; repeated calls cannot reset quotas.
         * @param tick Monotonic simulation tick. @return Number of queries accepted by Foundation this call.
         */
        [[nodiscard]] std::uint32_t Dispatch(std::uint64_t tick);
        /** @brief Publish a stable prefix of eligible terminal results at NavIntentCommit.
         * @param current Fresh exact Scene/root and active caller incarnations.
         * @return Number newly published; invalid/rollback observations leave publication unchanged.
         * @details Cancellation wins before publication. Expired pending work returns CapacityExceeded; eligible ready
         * results may publish on deadlineTick. Revision mismatch returns StaleSnapshot, never fabricated region proof.
         */
        [[nodiscard]] std::uint32_t Commit(const NavigationPathPublication &current);
        /** @brief Transfer one published result once. @param handle Exact request identity.
         * @return Owned terminal result or empty for pending, stale or already consumed handles.
         * @details A consumed slot is reclaimed only after its partition is quiescent; late workers cannot alias reuse.
         */
        [[nodiscard]] std::optional<NavigationPathCompletion> Take(NavRequestHandle handle);
        /** @brief Permanently close admission and cooperatively cancel unpublished requests without waiting. */
        void BeginShutdown() noexcept;
        /** @brief Report no retained request or worker partitions. @return True after terminal results are consumed. */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        struct State;
        explicit NavigationCoordinator(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Navigation
