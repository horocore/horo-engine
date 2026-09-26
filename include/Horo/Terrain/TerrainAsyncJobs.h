#pragma once

/**
 * @file TerrainAsyncJobs.h
 * @brief Owner-lane publication fence for bounded terrain and foliage background work.
 */

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Terrain/TerrainFoliageRegistry.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace Horo::Terrain {
    namespace Detail {
        struct TerrainAsyncWorkTag;
    }

    /** @brief Process-local identity of one accepted TerrainRuntime work item. */
    using TerrainAsyncWorkId = Foundation::Detail::NonZeroId64<Detail::TerrainAsyncWorkTag, TerrainErrors::IdentityInvalid>;

    /** @brief Exact runtime, content and registry publication captured before worker submission. */
    struct TerrainAsyncWorkFence final {
        TerrainRuntimeHandle runtime{};           /**< Exact live terrain incarnation. */
        TerrainSnapshotRevision revisions{};      /**< Independent content, residency, mutation and capability revisions. */
        TerrainFoliageRegistryBinding registry{}; /**< Exact immutable registry publication. */

        /** @brief Checks structural completeness. @return True when every identity and revision is valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return runtime.IsValid() && revisions.IsValid() && registry.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const TerrainAsyncWorkFence &) const noexcept = default;
    };

    /** @brief Feature-local pipeline that owns a background candidate. */
    enum class TerrainAsyncWorkKind : std::uint8_t {
        Cook,
        Load,
        EditPreview,
        Count,
    };

    /** @brief Owner-visible state; Prepared awaits a fenced owner-lane publication. */
    enum class TerrainAsyncWorkState : std::uint8_t {
        Queued,
        Running,
        Prepared,
        Succeeded,
        Failed,
        Cancelled,
    };

    /** @brief Finite admission limits selected by the host. */
    struct TerrainAsyncJobLimits final {
        static constexpr std::size_t HardMaximumTracked = 4'096;
        static constexpr std::uint64_t HardMaximumWorkUnits = 1'000'000'000;

        std::size_t maximumTracked{64};            /**< Accepted records retained until explicitly forgotten. */
        std::uint64_t maximumWorkUnits{1'000'000}; /**< Maximum declared units for one item. */

        /** @brief Rejects zero or above-hard-bound limits. @return True for finite usable limits. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumTracked > 0 && maximumTracked <= HardMaximumTracked && maximumWorkUnits > 0 &&
                   maximumWorkUnits <= HardMaximumWorkUnits;
        }
    };

    /**
     * @brief One immutable-input preparation and its owner-lane atomic publication.
     *
     * The worker callback must capture owned values or leases and must not change live
     * Terrain, editor, Scene or native consumer state. The publication callback runs
     * only from Advance on the creating owner thread after the exact fence is checked.
     * It must check cancellation and validate its candidate before making an atomic
     * authoritative change. Reentrant Advance or ReplaceFence is rejected while it runs.
     */
    struct TerrainAsyncWorkRequest final {
        std::uint64_t workUnits{};                                      /**< Finite declared preparation work. */
        TerrainFoliageCapabilitySet requiredCapabilities{};             /**< Exact required grants; no fallback. */
        CancellationToken parentCancellation{};                         /**< Parent operation cancellation ancestry. */
        std::optional<OperationId> operationId{};                       /**< Optional existing operation correlation. */
        std::optional<ConfigurationSnapshotRef> configuration{};        /**< Captured configuration when dependent. */
        ContextJobFunction prepare;                                     /**< Worker-only candidate preparation. */
        std::function<Result<void>(const CancellationToken &)> publish; /**< Owner-lane atomic publication. */
    };

    /** @brief Owned observation of one accepted item and its single terminal outcome. */
    struct TerrainAsyncWorkSnapshot final {
        TerrainAsyncWorkId id{};
        JobId jobId{};
        TerrainAsyncWorkKind kind{TerrainAsyncWorkKind::Cook};
        TerrainAsyncWorkFence fence{};
        TerrainAsyncWorkState state{TerrainAsyncWorkState::Queued};
        std::optional<Error> error{};

        /** @brief Tests whether the item has one immutable final outcome. @return True after success, failure or cancellation. */
        [[nodiscard]] constexpr bool IsTerminal() const noexcept {
            return state == TerrainAsyncWorkState::Succeeded || state == TerrainAsyncWorkState::Failed ||
                   state == TerrainAsyncWorkState::Cancelled;
        }
    };

    /**
     * @brief Host-composed TerrainRuntime owner-lane coordinator over Foundation jobs.
     *
     * The injected JobSystem outlives this coordinator. All methods except worker
     * callbacks run on the creating owner thread. Shutdown stops publication and
     * requests cancellation; IsDrained reports when accepted workers have terminated.
     */
    class TerrainAsyncJobs final {
    public:
        /**
         * @brief Creates one coordinator for a live terrain incarnation.
         * @param jobs Host-owned scheduler that outlives this coordinator.
         * @param fence Initial exact runtime and publication fence.
         * @param available Exact host capability grants.
         * @param limits Finite host admission limits.
         * @return Coordinator or a typed invalid descriptor failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<TerrainAsyncJobs>> Create(JobSystem &jobs, TerrainAsyncWorkFence fence,
                                                                              TerrainFoliageCapabilitySet available,
                                                                              TerrainAsyncJobLimits limits = {});

        /** @brief Requests cancellation and releases retained records; call on the creating owner lane after dependent leases are safe. */
        ~TerrainAsyncJobs();
        TerrainAsyncJobs(const TerrainAsyncJobs &) = delete;
        TerrainAsyncJobs &operator=(const TerrainAsyncJobs &) = delete;

        /**
         * @brief Admits bounded cook preparation against the current fence.
         * @param request Owned callbacks, cancellation ancestry and exact requirements.
         * @return Durable typed item ID, or a rejection with no item record.
         */
        [[nodiscard]] Result<TerrainAsyncWorkId> SubmitCook(TerrainAsyncWorkRequest request);
        /**
         * @brief Admits bounded decoded load preparation against the current fence.
         * @param request Owned callbacks, cancellation ancestry and exact requirements.
         * @return Durable typed item ID, or a rejection with no item record.
         */
        [[nodiscard]] Result<TerrainAsyncWorkId> SubmitLoad(TerrainAsyncWorkRequest request);
        /**
         * @brief Admits bounded edit-preview preparation against the current fence.
         * @param request Owned callbacks, cancellation ancestry and exact requirements.
         * @return Durable typed item ID, or a rejection with no item record.
         */
        [[nodiscard]] Result<TerrainAsyncWorkId> SubmitEditPreview(TerrainAsyncWorkRequest request);
        /**
         * @brief Observes a worker and publishes its candidate at this owner safe point if still current.
         * @param id Accepted item identity.
         * @return Current owned snapshot; a terminal outcome is immutable.
         */
        [[nodiscard]] Result<TerrainAsyncWorkSnapshot> Advance(TerrainAsyncWorkId id);
        /** @brief Reads the last owner-lane state without advancing or publishing it. @param id Accepted item identity. @return Last
         * snapshot or a typed unknown/affinity error. */
        [[nodiscard]] Result<TerrainAsyncWorkSnapshot> Snapshot(TerrainAsyncWorkId id) const;
        /** @brief Requests cooperative cancellation without turning a completed result into failure. @param id Accepted item identity.
         * @return Success or a typed unknown/affinity error. */
        [[nodiscard]] Result<void> RequestCancel(TerrainAsyncWorkId id);
        /**
         * @brief Replaces the exact publication fence and cancels all older candidates.
         * @param fence New incarnation and revision fence for the same stable dataset. Revisions cannot regress within an incarnation.
         * @param available New exact host capability grants.
         * @return Success or typed invalid/stale/active-publication owner failure without changing current state. Grant changes need a
         * new capability revision.
         */
        [[nodiscard]] Result<void> ReplaceFence(TerrainAsyncWorkFence fence, TerrainFoliageCapabilitySet available);
        /** @brief Closes admission and publication and requests cancellation of accepted work; owner-lane only and idempotent. */
        void BeginShutdown();
        /** @brief Reports whether every accepted Foundation job has terminated; never waits. @return True only on the owner lane after all
         * workers terminate. */
        [[nodiscard]] bool IsDrained() const;
        /** @brief Releases one terminal record after its Foundation job has terminated. @param id Accepted item identity. @return Success
         * or a typed not-ready/unknown/affinity error. */
        [[nodiscard]] Result<void> Forget(TerrainAsyncWorkId id);

    private:
        struct Impl;
        explicit TerrainAsyncJobs(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] Result<TerrainAsyncWorkId> Submit(TerrainAsyncWorkKind kind, TerrainAsyncWorkRequest request);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Terrain
