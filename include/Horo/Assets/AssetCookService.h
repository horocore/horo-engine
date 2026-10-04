#pragma once

/**
 * @file AssetCookService.h
 * @brief Bounded structured cook orchestration with cache reuse, cancellation, and fail-closed publication.
 */

#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Assets/CookCatalog.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Horo::Assets {

    /**
     * @brief All inputs needed to run one cook operation.
     */
    struct AssetCookRequest {
        std::filesystem::path sourceRoot;                    /**< Project source root for resolving relative source paths. */
        std::filesystem::path cacheRoot;                     /**< Cache root directory for immutable artifact cache. */
        std::filesystem::path cookedRoot;                    /**< Target root for generation publication. */
        AssetRegistrySnapshot registry;                      /**< Pinned immutable registry snapshot. */
        AssetCookTargetId target;                            /**< Cook target to produce artifacts for. */
        AssetCookLimits limits;                              /**< Bounded size and concurrency limits. */
        BuildOutputStore *buildOutputStore{nullptr};         /**< Optional typed per-asset cook output authority. */
        OperationStore *operationStore{nullptr};             /**< Optional user-facing operation authority. */
        std::function<void()> requestCancel;                 /**< Cooperative cancellation request paired with the supplied token. */
        std::shared_ptr<DurableFileSystem> publicationFiles; /**< Required host-owned durable writer and common native lock capability. */
        std::function<Result<AssetId>()> newPublicationOperationId; /**< Required host entropy source for unique publication staging. */
    };

    /**
     * @brief Aggregate result of one cook operation.
     */
    struct AssetCookReport {
        AssetCookGeneration generation; /**< Published generation. */
        std::size_t totalAssets{};      /**< Total assets in the registry snapshot. */
        std::size_t cookedAssets{};     /**< Assets cooked on this run (misses). */
        std::size_t cacheHits{};        /**< Assets served from cache. */
    };

    /**
     * @brief Bounded structured cook orchestrator.
     * @details Pins one immutable cooker catalog snapshot and one registry snapshot,
     *          then cooks all records in deterministic AssetId order using the job system.
     *          Fail-fast: first error cancels and joins all accepted siblings, preserving
     *          the prior active generation.
     */
    class AssetCookService final {
    public:
        /**
         * @brief Constructs a cook service with shared job system and catalog.
         * @param jobs Job system that outlives this service.
         * @param catalog Published immutable cooker catalog snapshot.
         */
        AssetCookService(JobSystem &jobs, std::shared_ptr<const CookerCatalogSnapshot> catalog);

        /**
         * @brief Cooks all assets in the request's registry snapshot for the given target.
         * @param request Cook operation inputs.
         * @param cancellation Parent operation cancellation token.
         * @return Published generation report, or a typed error (missing cooker, read failure, etc.).
         * @pre publicationFiles and newPublicationOperationId are supplied by the host, including for an empty registry.
         * @post Recovery, full-inventory replacement and success adoption hold the common .cook-writer.lock.
         * @post Cancellation before pointer replacement preserves the old generation; committed output remains successful.
         */
        [[nodiscard]] Result<AssetCookReport> Cook(const AssetCookRequest &request, const CancellationToken &cancellation);

    private:
        JobSystem &jobs_;
        std::shared_ptr<const CookerCatalogSnapshot> catalog_;
    };

}  // namespace Horo::Assets
