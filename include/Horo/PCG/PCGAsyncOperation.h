#pragma once

/**
 * @file PCGAsyncOperation.h
 * @brief Generation-fenced PCG background preparation and owner-lane completion.
 */

#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGGenerationPlan.h"

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace Horo::PCG {
    struct PCGAsyncSceneIdentityTag;
    struct PCGAsyncOperationIdentityTag;

    /** @brief Stable scene owner identity supplied by the host, not a scene pointer. */
    using PCGAsyncSceneId = PcgStableIdentity<PCGAsyncSceneIdentityTag>;
    /** @brief Process-local identity of one accepted PCG asynchronous operation. */
    using PCGAsyncOperationId = PcgStableIdentity<PCGAsyncOperationIdentityTag>;

    /** @brief Exact owner and content evidence captured before scheduling. */
    struct PCGAsyncFence final {
        PCGAsyncSceneId scene{};             /**< Scene whose lifetime owns the work. */
        GenerationCellId cell{};             /**< Exact streaming-cell owner. */
        GraphGeneration graph{};             /**< Exact authored graph revision. */
        Sha256Digest sourceDigest{};         /**< Digest of exact canonical graph source bytes. */
        std::uint64_t runtimeGeneration{};   /**< Never-reused host activation generation. */
        std::uint64_t planGeneration{};      /**< Exact cooked-plan publication; zero before cook or load. */
        std::uint64_t inputGeneration{};     /**< Exact immutable input publication. */
        std::uint64_t authorityGeneration{}; /**< Exact host/target authority publication. */

        /** @brief Checks required owner and revision evidence. @return True for a usable fence. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return scene.IsValid() && cell.IsValid() && graph.IsValid() && sourceDigest != Sha256Digest{} && runtimeGeneration != 0 &&
                   inputGeneration != 0 && authorityGeneration != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PCGAsyncFence &) const noexcept = default;
    };

    /** @brief Feature operation, independent of where its result will be committed. */
    enum class PCGAsyncKind : std::uint8_t {
        Cook,
        Load,
        Evaluate,
        Preview,
        Count
    };

    /** @brief Owner-visible state; only the last four values are terminal. */
    enum class PCGAsyncState : std::uint8_t {
        Queued,
        Running,
        CandidateReady,
        Succeeded,
        Failed,
        Cancelled,
        Stale
    };

    /** @brief Finite host-selected operation capacity and declared work envelope. */
    struct PCGAsyncLimits final {
        std::size_t maximumTracked{64};                                /**< Active and retained accepted records. */
        std::size_t maximumScopes{4'096};                              /**< Registered live/closed scene-cell-graph scopes. */
        std::uint64_t maximumWorkUnits{1'000'000};                     /**< Maximum declared units per operation. */
        Duration childJoinTimeout{Duration::FromMilliseconds(30'000)}; /**< Bounded worker-side child wait before cancellation/drain. */

        /** @brief Checks bounded, nonzero limits. @return True for a usable envelope. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumTracked > 0 && maximumTracked <= 4'096 && maximumScopes > 0 && maximumScopes <= 65'536 && maximumWorkUnits > 0 &&
                   maximumWorkUnits <= 1'000'000'000 && childJoinTimeout > Duration{} &&
                   childJoinTimeout <= Duration::FromMilliseconds(300'000);
        }
    };

    /**
     * @brief Worker-only adapter that owns and accounts for every accepted child job.
     *
     * The coordinator joins the entire group before observing preparation as complete.
     * A callback must not retain this borrowed adapter or any of its references.
     */
    class PCGAsyncPrepareContext final {
    public:
        PCGAsyncPrepareContext(const PCGAsyncPrepareContext &) = delete;
        PCGAsyncPrepareContext &operator=(const PCGAsyncPrepareContext &) = delete;

        /** @brief Returns the operation cancellation ancestry. @return Borrowed token valid during preparation. */
        [[nodiscard]] const CancellationToken &Cancellation() const noexcept;
        /** @brief Reports monotonic phase progress on the parent job. @param phase Bounded nonempty phase.
         * @param value Optional normalized progress. @return Foundation validation or lifecycle result. */
        [[nodiscard]] Result<void> UpdateProgress(std::string_view phase, std::optional<float> value = std::nullopt) const;
        /** @brief Submits a structured child owned by this operation. @param work Owned child callback.
         * @return Child job ID, or a typed rejection that creates no child. */
        [[nodiscard]] Result<JobId> SpawnChild(JobFunction work);

    private:
        friend class PCGAsyncOperations;
        PCGAsyncPrepareContext(const JobExecutionContext &job, TaskGroup &children, std::atomic_size_t &accepted) noexcept;

        const JobExecutionContext &job_;
        TaskGroup &children_;
        std::atomic_size_t &accepted_;
    };

    /** @brief Accepted operation callbacks; both must own their captured inputs or exact-generation leases. */
    struct PCGAsyncRequest final {
        PCGAsyncFence fence{};                                          /**< Must equal the registered current fence at admission. */
        std::uint64_t workUnits{};                                      /**< Complete declared preparation charge. */
        CancellationToken parentCancellation{};                         /**< Host or parent cancellation ancestry. */
        std::optional<OperationId> operationId{};                       /**< Correlation only; no OperationStore mutation. */
        std::optional<ConfigurationSnapshotRef> configuration{};        /**< Captured configuration when needed. */
        std::function<Result<void>(PCGAsyncPrepareContext &)> prepare;  /**< Pure worker preparation, including child work. */
        std::function<Result<void>(const CancellationToken &)> publish; /**< Owner-lane immutable PCG candidate/result publication only. */
    };

    /** @brief Single immutable outcome after all accepted child work and owner publication are accounted for. */
    struct PCGAsyncTerminalResult final {
        PCGAsyncState state{PCGAsyncState::Failed}; /**< Success, failure, cancellation, or stale rejection. */
        std::optional<Error> error{};               /**< Typed cause; empty only for success. */
        std::size_t acceptedChildren{};             /**< Every accepted child has terminated before this result exists. */
        std::uint64_t workUnits{};                  /**< Declared complete preparation charge. */
    };

    /** @brief Owned observation of an accepted operation; a published terminal never changes. */
    struct PCGAsyncSnapshot final {
        PCGAsyncOperationId id{};
        JobId jobId{};
        PCGAsyncKind kind{PCGAsyncKind::Cook};
        PCGAsyncFence fence{};
        PCGAsyncState state{PCGAsyncState::Queued};
        std::optional<PCGAsyncTerminalResult> terminal{};
    };

    /**
     * @brief Host-composed owner-lane PCG operation and publication coordinator.
     *
     * The injected scheduler outlives this object. All methods run on the creating
     * owner thread; worker callbacks never touch live target state. Invalidating
     * a scope closes its admission and publication, requests cancellation, and
     * retains leases until every accepted worker/child and owner completion drains.
     * The host must observe IsDrained before releasing dependent owners.
     * Scene and target commits are external transactions, never performed here.
     */
    class PCGAsyncOperations final {
    public:
        /** @brief Creates a coordinator with finite limits. @param jobs Process-owned scheduler.
         * @param limits Admission envelope. @return Coordinator or typed invalid-limit error. */
        [[nodiscard]] static Result<std::unique_ptr<PCGAsyncOperations>> Create(JobSystem &jobs, const PCGAsyncLimits &limits = {});
        ~PCGAsyncOperations();
        PCGAsyncOperations(const PCGAsyncOperations &) = delete;
        PCGAsyncOperations &operator=(const PCGAsyncOperations &) = delete;

        /** @brief Registers a previously absent scene/cell/graph scope. @param fence Initial exact publication.
         * @return Success or invalid/duplicate/closed error. */
        [[nodiscard]] Result<void> RegisterFence(const PCGAsyncFence &fence);
        /** @brief Atomically replaces a live scope's current fence and cancels older work.
         * @param fence New exact publication for the same scene/cell/graph lineage.
         * @return Success or typed invalid/stale/closed/reentrant error. */
        [[nodiscard]] Result<void> ReplaceFence(const PCGAsyncFence &fence);
        /** @brief Schedules pure work against one registered exact fence. @param kind Work kind.
         * @param request Owned callbacks and submission evidence. @return ID or rejection with no record. */
        [[nodiscard]] Result<PCGAsyncOperationId> Submit(PCGAsyncKind kind, PCGAsyncRequest request);
        /** @brief Advances a completed worker and publishes only on the current owner lane.
         * @param id Accepted operation. @return Owned snapshot, immutable once terminal. */
        [[nodiscard]] Result<PCGAsyncSnapshot> Advance(PCGAsyncOperationId id);
        /** @brief Nonblocking owner-lane sweep of every accepted completion after invalidation or shutdown.
         * @return Number of operations still awaiting worker/owner completion, or a typed owner error. */
        [[nodiscard]] Result<std::size_t> AdvanceAll();
        /** @brief Reads the last owner-lane state without publishing. @param id Accepted operation.
         * @return Owned snapshot or typed unknown/wrong-thread error. */
        [[nodiscard]] Result<PCGAsyncSnapshot> Snapshot(PCGAsyncOperationId id) const;
        /** @brief Requests cooperative cancellation without changing an already terminal result.
         * @param id Accepted operation. @return Success or typed unknown/wrong-thread error. */
        [[nodiscard]] Result<void> RequestCancel(PCGAsyncOperationId id);
        /** @brief Closes all graph work in the exact scene/cell. @return Success or wrong-thread/reentrant error. */
        [[nodiscard]] Result<void> InvalidateGraph(PCGAsyncSceneId scene, GenerationCellId cell, GraphId graph);
        /** @brief Closes all graph work in one scene/cell. @return Success or wrong-thread/reentrant error. */
        [[nodiscard]] Result<void> InvalidateCell(PCGAsyncSceneId scene, GenerationCellId cell);
        /** @brief Closes all cell work in one scene. @return Success or wrong-thread/reentrant error. */
        [[nodiscard]] Result<void> InvalidateScene(PCGAsyncSceneId scene);
        /** @brief Closes host admission/publication and cancels every unfinished operation; idempotent on owner lane. */
        [[nodiscard]] Result<void> BeginShutdown();
        /** @brief Reports completion of every accepted worker and structured child, without blocking. */
        [[nodiscard]] bool IsDrained() const;
        /** @brief Reports completed owner and worker drain for one exact graph scope. */
        [[nodiscard]] bool IsGraphDrained(PCGAsyncSceneId scene, GenerationCellId cell, GraphId graph) const;
        /** @brief Reports completed owner and worker drain for one scene/cell scope. */
        [[nodiscard]] bool IsCellDrained(PCGAsyncSceneId scene, GenerationCellId cell) const;
        /** @brief Reports completed owner and worker drain for one scene scope. */
        [[nodiscard]] bool IsSceneDrained(PCGAsyncSceneId scene) const;
        /** @brief Releases a closed graph scope after every operation record was forgotten.
         * @return Success or typed wrong-thread/unknown/not-ready error. */
        [[nodiscard]] Result<void> RetireScope(PCGAsyncSceneId scene, GenerationCellId cell, GraphId graph);
        /** @brief Releases an immutable terminal after its entire worker group drained.
         * @param id Accepted operation. @return Success or typed not-ready/unknown error. */
        [[nodiscard]] Result<void> Forget(PCGAsyncOperationId id);

    private:
        struct Impl;
        explicit PCGAsyncOperations(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] static Result<void> RunPreparation(const JobExecutionContext &job, JobSystem &scheduler,
                                                         const std::function<Result<void>(PCGAsyncPrepareContext &)> &prepare,
                                                         std::atomic_size_t &acceptedChildren, Duration childJoinTimeout);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::PCG
