#pragma once

/** @file NavigationRuntimeRebuildJobs.h
 * @brief Owner-lane admission and transfer of a detached runtime tile rebuild.
 */

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Navigation/NavigationRuntimeTileRebuild.h"

namespace Horo::Navigation {
    /** @brief Owns the immutable inputs and tile pins that must outlive asynchronous preparation. */
    struct NavigationRuntimeOwnedRebuildRequest final {
        NavigationRuntimeRebuildFence expected;
        NavigationGeneration nextTopology;
        std::shared_ptr<const NavigationBakeInputSnapshot> input;
        std::vector<NavigationBakeTile> layout;
        std::vector<std::shared_ptr<const NavigationCookedTile>> activeTiles;
        NavigationTileBakeCompatibility compatibility;
        NavigationTileBuildLimits tileLimits{};
        std::size_t maximumRebuiltTiles{32};
        std::size_t maximumOwnedBytes{64U * 1024U * 1024U};
    };

    /** @brief Owns one accepted runtime rebuild and its unpublished result on the Scene owner lane.
     * @details A running, cancelled or completed job holds the slot until its terminal result is taken. The scheduler
     * outlives submission. Worker captures own their inputs and providers and never borrow this owner or the Scene.
     */
    class NavigationRuntimeRebuildJobs final {
    public:
        /** @brief Binds the explicitly composed job scheduler and preparation providers.
         * @param jobs Process job system used for bounded CPU submission.
         * @param builder Builder with worker-safe const preparation operations.
         * @param factory Query materializer with worker-safe const preparation operations.
         */
        NavigationRuntimeRebuildJobs(const JobSystem &jobs, std::shared_ptr<const INavigationMeshBuilder> builder,
                                     std::shared_ptr<const INavigationRuntimeRebuildBackendFactory> factory) noexcept;
        NavigationRuntimeRebuildJobs(const NavigationRuntimeRebuildJobs &) = delete;
        NavigationRuntimeRebuildJobs &operator=(const NavigationRuntimeRebuildJobs &) = delete;
        /** @brief Requests cancellation and releases owner state without waiting on the worker. */
        ~NavigationRuntimeRebuildJobs();

        /** @brief Admits a single detached rebuild on the background CPU queue.
         * @param request Owned capture. Providers never observe subsequent owner edits.
         * @param parentCancellation Optional Scene or command cancellation ancestry.
         * @param reservation Optional exclusive Scene budget matching the request fence and maximum owned bytes.
         * @return Success or typed invalid, cancelled or admission failure. Rejection leaves any existing job untouched.
         */
        [[nodiscard]] Result<void> Start(NavigationRuntimeOwnedRebuildRequest request, const CancellationToken &parentCancellation = {},
                                         NavigationRuntimeRebuildReservation reservation = {});
        /** @brief Transfers the terminal result once without waiting or publishing.
         * @return Empty when idle or unfinished, the prepared candidate, or an owned typed terminal failure.
         */
        [[nodiscard]] Result<std::optional<NavigationRuntimeTileRebuildCandidate>> TakeCompleted();
        /** @brief Cancels a pending or completed unpublished result while retaining its slot until terminal transfer. */
        void Cancel() const noexcept;
        /** @brief Reports whether the owner still holds an accepted job. @return True until terminal transfer. */
        [[nodiscard]] bool Busy() const noexcept;

    private:
        struct Pending;
        const JobSystem &jobs_;
        std::shared_ptr<const INavigationMeshBuilder> builder_;
        std::shared_ptr<const INavigationRuntimeRebuildBackendFactory> factory_;
        std::unique_ptr<Pending> pending_;
    };
}  // namespace Horo::Navigation
