#pragma once

#include "Horo/Navigation/NavigationCoordinator.h"
#include "NavigationBoundedQueue.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace Horo::Navigation {
    /** @brief Shared callback lifetime; request/partition mutation is owner-only except immutable running buffers and completion
     * publication. */
    struct NavigationCoordinator::State final {
        struct Entry final {
            NavigationPathSubmission submission;
            CancellationSource cancellation;
            NavigationQueuedQuery query;
            std::optional<NavigationPathCompletion> candidate;
            std::optional<NavigationPathCompletion> terminal;
            JobId job{};
            std::uint64_t admittedTick{};
            bool executing{};
            bool published{};
            bool taken{};
        };

        struct Slot final {
            std::uint32_t generation{1};
            std::optional<Entry> entry;
        };

        struct Work final {
            NavigationQueuedQuery query;
            NavigationPathCaller caller;
            NavigationOutcomeProvenance source;
            std::optional<ConfigurationSnapshotRef> configuration;
        };

        struct Batch final {
            std::vector<Work> work;
            std::vector<std::optional<NavigationPathCompletion>> fallback;
            std::optional<JobHandle> job;
        };

        struct Quota final {
            NavigationDynamicOwnerId caller;
            std::uint32_t dispatched{};
            std::uint64_t nodes{};
        };

        /** @brief Prepare bounded storage before owner scheduling. */
        State(JobSystem &scheduler, const NavigationPathBatchLimits &bounds);
        /** @brief Resolve an exact retained generation on the owner. */
        [[nodiscard]] Entry *Find(NavRequestHandle handle) noexcept;
        /** @brief Select one fair quota-admissible request and reserve one bounded storage scan. */
        [[nodiscard]] std::optional<std::uint32_t> Select(std::uint64_t tick, std::uint64_t previousCaller, const Batch &staged) noexcept;
        /** @brief Check one pinned root and compatible unconfigured execution context. */
        [[nodiscard]] bool Compatible(const Work &work, const Entry &entry) const noexcept;
        /** @brief Count conservative caller request reservations including staged work. */
        [[nodiscard]] std::uint32_t Used(NavigationDynamicOwnerId caller, const Batch &staged) const noexcept;
        /** @brief Acquire all currently published candidates without mutating running partition buffers. */
        void CollectCompletions();
        /** @brief Drain completion transport and reclaim only terminal partitions. */
        void Collect();
        /** @brief Prepare one compatible fair partition without reserving scheduler authority. */
        void PrepareBatch(std::uint32_t index, std::uint64_t tick);
        /** @brief Stage one compatible candidate while preserving provider scratch limits. */
        [[nodiscard]] bool StageNext(Batch &batch, std::uint64_t tick, std::uint64_t &caller);
        /** @brief Reclaim one terminal partition after its transport records are acquired. */
        void CollectBatchWork(Batch &batch, const JobSnapshot &snapshot);
        /** @brief Submit prepared work nonblocking and charge quotas only after Foundation accepts it. */
        [[nodiscard]] static bool SubmitBatch(const std::shared_ptr<State> &state, std::uint32_t index);
        /** @brief Check conservative global/caller node reservations for one candidate. */
        [[nodiscard]] bool WithinNodeBudget(const Entry &entry, const Batch &staged) const noexcept;
        /** @brief Validate one bounded ordered authority slice and exact current activation/root before mutation. */
        [[nodiscard]] bool ValidPublication(const NavigationPathPublication &current) const noexcept;
        /** @brief Validate cancellation and owner-phase authority before terminal publication. */
        [[nodiscard]] static std::optional<Error> PublicationFailure(const Entry &entry, const NavigationPathPublication &current);
        /** @brief Perform the sole exactly-once terminal transition after owner validation. */
        static void Publish(Entry &entry, std::optional<Error> failure, std::uint64_t tick);
        /** @brief Retire a consumed quiescent slot without generation wrap. */
        static void Retire(Slot &slot) noexcept;
        /** @brief Reserve per-tick caller/global node and request quota. */
        void Charge(NavigationDynamicOwnerId caller, std::uint32_t nodes);
        /** @brief Execute immutable batch work only on a Foundation callback. */
        [[nodiscard]] static Result<void> Execute(const std::shared_ptr<State> &state, std::uint32_t batch,
                                                  const CancellationToken &cancellation);
        /** @brief Retain exact source and provider result in one owned record. */
        [[nodiscard]] static NavigationPathCompletion Complete(const Work &work, JobId job, Result<NavigationPath> result);
        /** @brief Compare every captured combined-root fence except publication tick. */
        [[nodiscard]] static bool SameSource(const NavigationOutcomeProvenance &a, const NavigationOutcomeProvenance &b) noexcept;
        /** @brief Reject provider outputs exceeding the admitted shape and bounds. */
        [[nodiscard]] static bool ValidResult(const NavigationPathRequest &request, const NavigationPath &path) noexcept;

        JobSystem &jobs;
        NavigationPathBatchLimits limits;
        std::vector<Slot> slots;
        std::vector<Batch> batches;
        Detail::BoundedMpmcQueue<NavigationPathCompletion> completions;
        std::vector<Quota> quotas;
        std::vector<std::uint32_t> publicationOrder;
        std::uint64_t sequence{};
        std::uint64_t lastCaller{};
        std::uint64_t tick{};
        std::optional<std::uint64_t> dispatchTick;
        std::uint32_t dispatched{};
        std::uint64_t nodes{};
        std::uint32_t selectionProbes{};
        bool closed{};
    };
}  // namespace Horo::Navigation
