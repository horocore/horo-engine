#pragma once

/** @file NavigationBakeService.h
 * @brief Host-composed incremental navigation cooking with bounded latest-request ownership.
 */

#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Navigation/NavigationBakeJobs.h"
#include "Horo/Navigation/NavigationTileArtifact.h"

namespace Horo::Application {
    /** @brief Fixed host composition for one definition, project output root and cook target. */
    struct NavigationBakeServiceConfig {
        Assets::AssetId definition;
        Assets::AssetTypeId artifactType;
        AssetCookTargetId target;
        std::filesystem::path cacheRoot;
        std::filesystem::path targetRoot;
        std::shared_ptr<const Navigation::INavigationMeshBuilder> builder;
        std::shared_ptr<DurableFileSystem> files;
        Navigation::NavigationBakeJobBudget budget;
        Navigation::NavigationTileBuildLimits tileLimits;
        Assets::AssetCookLimits cookLimits;
        std::size_t maximumTiles{1024};
        std::size_t maximumCandidateBytes{64U * 1024U * 1024U};
    };

    /** @brief Complete owned capture; each submission replaces the entire definition tile closure. */
    struct NavigationBakeRequest {
        std::shared_ptr<const Navigation::NavigationBakeInputSnapshot> input;
        Navigation::NavigationTileBakeCompatibility compatibility;
        std::vector<Navigation::NavigationBakeTile> tiles;
        std::vector<Navigation::NavigationSourceObservation> sources; /**< Complete current authoritative source observations. */
        CancellationToken cancellation;
    };

    /** @brief Last successfully published immutable generation; retained readers survive subsequent submissions. */
    struct NavigationBakePublication {
        Assets::AssetCookGeneration generation;
        Navigation::NavigationCookedTileSet tiles;
        std::size_t rebuiltTiles{};
        std::size_t reusedTiles{};
    };

    namespace NavigationBakeDetail {
        struct ServiceState;
        struct Attempt;
    }  // namespace NavigationBakeDetail

    /** @brief Single host-thread admission authority with one active and one replaceable pending request.
     * @note Host calls Submit/Invalidate/Pump/Close on one thread; jobs and operations outlive all drained work.
     * Every source edit must submit a coherent capture or call Invalidate before it becomes current.
     * Workers own captures and service state; they never retain the facade or host stack references.
     */
    class NavigationBakeService final {
        struct ConstructionKey {
        private:
            friend class NavigationBakeService;
            ConstructionKey() = default;
        };

    public:
        /** @brief Internal factory-only construction; the private key prevents bypassing Create validation.
         * @param state Validated worker-owned service composition. @param operations Process operation authority.
         * @param jobs Process scheduler that outlives accepted work.
         */
        NavigationBakeService(ConstructionKey, std::shared_ptr<NavigationBakeDetail::ServiceState> state, OperationStore &operations,
                              JobSystem &jobs);
        /** @brief Creates an explicit composition; no backend is discovered or selected.
         * @param config Fixed validated host configuration. @param operations Process operation authority.
         * @param jobs Process scheduler with at least two workers. @return Owned service or typed admission failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<NavigationBakeService>> Create(NavigationBakeServiceConfig config,
                                                                                   OperationStore &operations, JobSystem &jobs);
        /** @brief Closes admission; process owners drain accepted jobs before destroying the scheduler and operation store. */
        ~NavigationBakeService();
        NavigationBakeService(const NavigationBakeService &) = delete;
        NavigationBakeService &operator=(const NavigationBakeService &) = delete;
        /** @brief Queues a complete replacement, cancelling older unadopted work without waiting.
         * @param request Owned immutable current input, compatibility and sorted unique tile layout.
         * @return Stable queued operation identity or typed invalid/admission error.
         */
        [[nodiscard]] Result<OperationId> Submit(NavigationBakeRequest request);
        /** @brief Invalidates unadopted work after a source edit without requesting another bake. */
        void Invalidate() noexcept;
        /** @brief Starts pending work after the previous operation drains; host polls without waiting. */
        void Pump();
        /** @brief Closes admission and cancels unadopted work; the host subsequently drains its scheduler. */
        void Close() noexcept;
        /** @brief Returns the last published generation lease from any thread. @return Null before first success. */
        [[nodiscard]] std::shared_ptr<const NavigationBakePublication> Published() const noexcept;

    private:
        std::shared_ptr<NavigationBakeDetail::ServiceState> state_;
        OperationStore &operations_;
        JobSystem &jobs_;
        std::shared_ptr<NavigationBakeDetail::Attempt> pending_;
        std::shared_ptr<NavigationBakeDetail::Attempt> active_;
        Navigation::NavigationBakeJobHandle activeJob_;
        std::uint64_t nextGeneration_{1};
        bool closed_{};
    };
}  // namespace Horo::Application
