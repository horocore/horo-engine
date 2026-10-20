#pragma once

/** @file NavigationBakeService.h
 * @brief Host-composed incremental navigation cooking with bounded latest-request ownership.
 */

#include "Horo/Application/NavigationBakeDiagnostics.h"
#include "Horo/Application/NavigationBakeSourceAuthority.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Navigation/NavigationBakeJobs.h"
#include "Horo/Navigation/NavigationBakeSourceValidation.h"
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
        std::shared_ptr<NavigationBakeSourceAuthority>
            sourceAuthority; /**< Required current-source authority; shared with host mutations. */
        Duration writerWaitTimeout{Duration::FromMilliseconds(30'000)}; /**< Background-only bounded lock contention deadline. */
        std::function<Result<Assets::AssetId>()> newOperationId; /**< Required host-composed operation UUID generator using OS entropy. */
        std::shared_ptr<NavigationBakeDiagnostics>
            diagnostics; /**< Project-owned retained output/history consumer shared by bake entry points. */
    };

    /** @brief Complete owned capture; each submission replaces the entire definition tile closure.
     * @details Empty tiles require an input with no partitions and complete current source authority. Publication
     * emits an explicit HNS3 empty replacement; it does not retain old tiles or invoke a tile builder. Bound empty
     * partitions still require their complete tile layout. ValidateOnly never changes the published generation.
     */
    struct NavigationBakeRequest {
        std::shared_ptr<const Navigation::NavigationBakeInputSnapshot> input;
        Navigation::NavigationTileBakeCompatibility compatibility;
        std::vector<Navigation::NavigationBakeTile> tiles;
        std::vector<Navigation::NavigationSourceObservation> sources; /**< Complete current authoritative source observations. */
        CancellationToken cancellation;
        std::vector<NavigationDiagnosticSource>
            diagnosticSources; /**< Host-resolved authored ownership for the captured source observations. */
        std::optional<Navigation::NavigationProjectProfile> projectProfile; /**< Owned release policy; omission remains non-release only. */
        /** @brief Optional exact scope-qualified surface rebuild selection over the complete captured definition.
         * Absence allows rebuilding every surface. A present value must be nonempty, unique and name surfaces
         * in the complete authored surface catalog. Submission canonicalizes its order. Every unselected tile must resolve to verified
         * current retained/cache content before any selected tile is built; otherwise the attempt fails without publication. Each key
         * preserves its Scene UUID; an unscoped key cannot select a matching surface in a Scene. Input, tiles and source observations
         * always describe the complete replacement closure, never a filtered subset.
         */
        std::optional<std::vector<Navigation::NavigationBakeSurfaceKey>> rebuildSurfaces;
        /** @brief Validates the same capture/layout and unselected output without builder/cache writes or publication.
         * A successful validation does not change Published or certify an older generation as current.
         */
        Navigation::NavigationBakeExecutionMode execution{Navigation::NavigationBakeExecutionMode::BuildAndPublish};
        /** @brief Complete authored surface catalog, including disabled surfaces, including surfaces without current tiles.
         * @details Each exact Scene-qualified key occurs once. Every tile belongs to this catalog. Empty surfaces
         * can be explicitly selected to remove their old topology; removing an unselected old tile instead fails
         * BakeSelectionIncomplete. Hosts capture this catalog from authored state, never from surviving geometry.
         * Submission sorts this bounded table. An empty catalog is valid only for a complete zero-tile closure.
         */
        std::vector<Navigation::NavigationBakeSurfaceKey> authoredSurfaces;
        /** @brief Owned external-source verification, required by file-backed host composition.
         * @details Absence denotes an in-memory capture protected solely by sourceAuthority. File-backed commands
         * must supply their retained source owner; the service never discovers or fabricates one. Verification runs
         * only on workers before gathering, at validate-only completion and under the final source-adoption lease.
         * Requests can join only when this exact immutable validation owner also matches.
         */
        std::shared_ptr<const Navigation::INavigationBakeSourceValidation> sourceValidation;
    };

    /** @brief Last successfully published immutable generation; retained readers survive subsequent submissions. */
    struct NavigationBakePublication {
        Assets::AssetCookGeneration generation;
        Navigation::NavigationCookedTileSet tiles;
        std::size_t rebuiltTiles{};
        std::size_t reusedTiles{};
    };

    /** @brief Resolves a verified complete definition artifact through the sole AssetCook current pointer.
     * @param config Host-chosen definition/type/target namespace and hard bounds; no builder is invoked.
     * @return Immutable decoded publication lease, or typed missing/corrupt/target/format failure.
     * @note Retained content is owned in memory and immutable generation files are retained across replacement.
     * Runtime/editor hosts pass these verified tiles into their ordinary provider activation transaction.
     */
    [[nodiscard]] Result<std::shared_ptr<const NavigationBakePublication>> ResolveNavigationBakePublication(
        const NavigationBakeServiceConfig &config);

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
