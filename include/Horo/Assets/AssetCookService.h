#pragma once

/**
 * @file AssetCookService.h
 * @brief Bounded structured cook orchestration with cache reuse, cancellation, and fail-closed publication.
 */

#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"
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
#include <string_view>
#include <vector>

namespace Horo::Assets {
    namespace CookErrors {
        /** @brief Cooperative cancellation acknowledged by AssetCook or its input capture. */
        extern const ErrorCodeDescriptor Cancelled;
        /** @brief The host's bounded operation store cannot admit this cook. */
        extern const ErrorCodeDescriptor OperationAdmissionFailed;
    }  // namespace CookErrors

    class AssetCookKeyProjection;

    /** @brief Validated unpublished resource envelope, borrowed only until the dependent-catalog callback returns. */
    struct AssetCookCandidateArtifactView final {
        AssetCookDependencyIdentity identity;
        std::span<const std::uint8_t> envelope;
    };

    /** @brief Immutable generic resource-before-dependent composition inside a single joined cook/publication. */
    struct AssetCookDependentPhase final {
        std::vector<AssetId> resourceIds; /**< Unique first-phase registry IDs; bounded by maximumAssets. */
        std::function<Result<std::shared_ptr<const CookerCatalogSnapshot>>(std::span<const AssetCookCandidateArtifactView>,
                                                                           const CancellationToken &)>
            makeCatalog; /**< Synchronous control-thread callback after first-phase jobs join. Views refer to the exact
                          envelopes retained for final publication, not a previous generation/cache directory.
                          Returned strategies own all retained evidence. Remaining V2 keys automatically bind these hashes.
                          No intermediate generation is published; cancellation/failure discards the entire candidate. */
    };

    /** @brief Optional synchronous projection of the existing cook operation onto a tooling protocol.
     * The host keeps the context
     * alive through the joined cook and supplies an OperationStore.
     * Observers consume stable phase text only, do not reenter the
     * cook, and cannot fail publication.
     */
    struct AssetCookProgressSink final {
        void *context{};
        void (*report)(void *context, OperationId operation, std::string_view phase, float completion) noexcept {};
    };

    /**
     * @brief All inputs needed to run one cook operation.
     */
    struct AssetCookRequest {
        std::filesystem::path sourceRoot;                       /**< Project source root for resolving relative source paths. */
        std::shared_ptr<const ImmutableFileStorage> cacheFiles; /**< Required host-owned immutable cache namespace. */
        std::filesystem::path cookedRoot;                       /**< Target root for generation publication. */
        AssetRegistrySnapshot registry;                         /**< Pinned immutable registry snapshot. */
        AssetCookTargetId target;                               /**< Cook target to produce artifacts for. */
        AssetCookLimits limits;                                 /**< Bounded size and concurrency limits. */
        BuildOutputStore *buildOutputStore{nullptr};            /**< Optional typed per-asset cook output authority. */
        OperationStore *operationStore{nullptr};                /**< Optional user-facing operation authority. */
        std::function<void()> requestCancel;                    /**< Cooperative cancellation request paired with the supplied token. */
        std::shared_ptr<DurableFileSystem> publicationFiles; /**< Required host-owned durable writer and common native lock capability. */
        std::function<Result<AssetId>()> newPublicationOperationId; /**< Required host entropy source for unique publication staging. */
        std::shared_ptr<const AssetCookInputSnapshot> pinnedInputs; /**< Optional real-file capture: selected registry records
                                                                   must match it; complete captured closure participates in V2 keys.
                                                                   Host retains project read authority through publication. */
        std::shared_ptr<const AssetCookKeyProjection>
            keyProjection; /**< Optional complete producer-local key selections; requires the exact pinnedInputs. */
        std::function<Result<void>()> validateHostInputs;              /**< Optional synchronous host compatibility/package/reload fence,
                                                                       called before work and immediately before pointer replacement.
                                                                       Captures must outlive this joined Cook invocation. */
        std::shared_ptr<const AssetCookDependentPhase> dependentPhase; /**< Optional bounded generic extension; requires pinnedInputs.
                                                                      Does not select prefab roots, templates or providers. */
        AssetCookProgressSink progress; /**< Borrowed same-operation progress, excluded from cook identity. */
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

    namespace Detail {
        class CookOperationScope;
    }

    /** @brief One admitted operation and output session shared by host preparation and AssetCook publication.
     * @details The control thread owns this move-only activity. Borrowed stores outlive it; the cancellation token is retained.
     * Dropping an unfinished activity terminates its admission. Only AssetCook can report authoritative publication success.
     */
    class AssetCookActivity final {
    public:
        /** @brief Admits one cook activity before the host begins fallible preparation.
         * @param request Source of the optional stores and cooperative cancellation request.
         * @param cancellation Token observed by both preparation and publication.
         * @param title User-facing operation title.
         * @param phase Initial stable preparation stage identifier.
         * @return Owned activity, or a typed projection admission failure before any host preparation.
         */
        [[nodiscard]] static Result<AssetCookActivity> Begin(const AssetCookRequest &request, const CancellationToken &cancellation,
                                                             std::string title, std::string phase);
        /** @brief Transfers the single terminal projection authority to another control scope. */
        AssetCookActivity(AssetCookActivity &&other) noexcept;
        /** @brief Releases an unfinished admission as a terminal operation. */
        ~AssetCookActivity();

        /** @brief Updates the active preparation stage and progress fraction.
         * @param phase Stable preparation stage.
         * @param message User-facing progress text.
         * @param progress Completion fraction between 0 and 1.
         */
        void Update(std::string phase, std::string message, float progress);
        /** @brief Completes a failed preparation with safe display text and an already redacted diagnostic.
         * @param error Typed redacted error copied into operation history.
         * @param record Failed or cancelled output evidence. Session and operation identities are assigned by the activity.
         */
        void FailPreparation(Error error, BuildOutputRecord record) noexcept;

    private:
        friend class AssetCookService;
        /** @brief Takes the privately admitted projection scope. */
        explicit AssetCookActivity(std::unique_ptr<Detail::CookOperationScope> scope) noexcept;
        std::unique_ptr<Detail::CookOperationScope> scope_;
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
         * @pre cacheFiles, publicationFiles and newPublicationOperationId are supplied by the host, including for an empty registry.
         * @post Recovery, full-inventory replacement and success adoption hold the common .cook-writer.lock.
         * @post Cancellation before pointer replacement preserves the old generation; committed output remains successful.
         */
        [[nodiscard]] Result<AssetCookReport> Cook(const AssetCookRequest &request, const CancellationToken &cancellation);

        /** @brief Publishes through the ordinary cook pipeline using an already admitted preparation activity.
         * @param request Fully prepared cook inputs using the same projection stores.
         * @param activity Sole operation/session authority and the original cancellation token.
         * @return Published generation or typed failure. The activity terminates by return, including when validation fails.
         */
        [[nodiscard]] Result<AssetCookReport> CookPrepared(const AssetCookRequest &request, AssetCookActivity activity);

    private:
        JobSystem &jobs_;
        std::shared_ptr<const CookerCatalogSnapshot> catalog_;
    };

}  // namespace Horo::Assets
