#pragma once

/** @file TerrainTileResourceOwner.h
 * @brief Renderer-owner staging and reader-safe retirement of terrain tile resources. */

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/TerrainRenderIntegration/TerrainRenderSnapshot.h"

#include <thread>

namespace Horo::TerrainRender {
    /** @brief Exact renderer-owned tile allocation; empty meshes represent fully holed tiles. */
    struct TerrainTileRenderResources final {
        Terrain::TerrainTileId tile{};
        Render::ResourceCreation<Render::RenderBufferHandle> vertices{}, indices{};
        Render::ResourceCreation<Render::RenderMeshHandle> mesh{};
    };

    /** @brief Immutable published resource lease; CPU readers pin its full generation until release. */
    struct TerrainTileResourceGeneration final {
        TerrainRenderExtractionSnapshot source;
        std::vector<TerrainTileRenderResources> tiles;
        std::uint64_t publication{};
    };

    /** @brief Creating render-owner lane manages bounded candidates; canonical frontend owns all native resources.
     * @details The frontend must outlive this owner and every issued reader. Host safe points process frontend uploads
     * separately, then call Advance. No normal-frame wait occurs. Old reader leases block logical resource release;
     * frontend graph/submission pins block native destruction after logical release. Host must close and drain this
     * owner before frontend shutdown; device-loss errors preserve incomplete retirement instead of claiming drain.
     */
    class TerrainTileResourceOwner final {
    public:
        /** @brief Composes a bounded owner without backend discovery or activation.
         * @param frontend Initialized selected renderer borrowed through explicit host composition.
         * @param maximumRetiring Positive old-generation ceiling, at most sixteen.
         * @throws std::invalid_argument If the finite retirement ceiling is invalid.
         * @throws std::bad_alloc Bounded retirement metadata allocation fails. */
        explicit TerrainTileResourceOwner(Render::RenderFrontend &frontend, std::uint32_t maximumRetiring = 4);
        /** @brief Destroys only after closed admission, released reader leases and complete logical drain. */
        ~TerrainTileResourceOwner();
        TerrainTileResourceOwner(const TerrainTileResourceOwner &) = delete;
        TerrainTileResourceOwner &operator=(const TerrainTileResourceOwner &) = delete;

        /** @brief Queues owned vertex/index uploads for one detached candidate, preserving active resources.
         * @param source Complete immutable generation from capture.
         * @param current Exact current Terrain owner evidence.
         * @param expectedPublication Zero for insert, otherwise exact current resource publication.
         * @param cancellation Cooperative cancellation checked before and throughout queuing.
         * @return Success or original typed admission failure; partial queued resources remain owned for retirement.
         * @pre Host retains exact material/artifact bindings separately; this owner realizes geometry only.
         * @details Bounded load/preparation work. Source request generations strictly advance and semantic
         * revisions never regress. Peak new/old resources are charged by the canonical frontend memory envelope.
         * @throws std::bad_alloc Detached metadata allocation fails before resource queuing. */
        [[nodiscard]] Result<void> Stage(TerrainRenderExtractionSnapshot source, const TerrainRenderSnapshotHeader &current,
                                         std::uint64_t expectedPublication, const CancellationToken &cancellation = {});
        /** @brief Advances ready buffers to meshes and publishes only a complete ready generation.
         * @param current Exact current Terrain evidence; changed evidence aborts only the candidate.
         * @param cancellation Cancellation wins before publication.
         * @return True when committed, false while uploads are pending, or typed rejection preserving active data.
         * @pre Host processes frontend upload batches on its render safe-point lane before calling this method. */
        [[nodiscard]] Result<bool> Advance(const TerrainRenderSnapshotHeader &current, const CancellationToken &cancellation = {});
        /** @brief Captures the active generation without allocation.
         * @return Immutable reader lease, empty before first publication, or wrong-thread/closed failure. */
        [[nodiscard]] Result<std::shared_ptr<const TerrainTileResourceGeneration>> Snapshot() const;
        /** @brief Cancels pending work and closes new admission idempotently; readers remain valid.
         * @return Success or wrong-thread failure. Caller must subsequently drain and release readers. */
        [[nodiscard]] Result<void> BeginShutdown();
        /** @brief Releases only reader-free generations through frontend deferred retirement.
         * @return Number of logical generations released, or original frontend failure retaining unreleased handles.
         * @details Bounded by the constructor ceiling plus one detached candidate; no GPU wait or forced reader release. */
        [[nodiscard]] Result<std::size_t> DrainRetired();
        /** @brief Reports logical integration drain, independently of native frontend completion.
         * @return True only after shutdown with no current, pending, or retiring generations. */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        [[nodiscard]] Result<void> CheckOwner() const;
        void DiscardPending() noexcept;
        [[nodiscard]] Result<void> Release(TerrainTileResourceGeneration &generation);
        Render::RenderFrontend &frontend_;
        std::thread::id owner_;
        std::uint32_t maximumRetiring_;
        bool closed_{};
        std::uint64_t lastPublication_{};
        std::shared_ptr<TerrainTileResourceGeneration> current_, pending_;
        std::vector<std::shared_ptr<TerrainTileResourceGeneration>> retiring_;
    };
}  // namespace Horo::TerrainRender
