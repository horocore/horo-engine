#pragma once

/**
 * @file AssetReimport.h
 * @brief Identity-preserving transactional reimport of one project asset.
 */

#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Platform.h"

#include <filesystem>
#include <vector>

namespace Horo::Assets {
    /** @brief Validated inputs required to reimport one committed project asset. */
    struct AssetReimportRequest {
        std::filesystem::path absoluteProjectRoot;             /**< Canonical absolute project root. */
        std::filesystem::path absoluteAssetPath;               /**< Absolute committed asset payload path. */
        const AssetImporterCatalogSnapshot *importerCatalog{}; /**< Pinned importer catalog. */
        AssetRegistry *registry{};                             /**< Owner-thread registry publisher. */
        DurableFileSystem *files{};                            /**< Durable same-filesystem publication service. */
    };

    /** @brief Result of a successful identity-preserving reimport transaction. */
    struct AssetReimportReport {
        AssetId assetId;                        /**< Stable identity preserved by the transaction. */
        std::vector<AssetImportReason> reasons; /**< Detected reasons recorded in metadata. */
        std::string sourceHash;                 /**< Newly committed source hash. */
        std::string importerVersion;            /**< Newly committed importer version. */
        std::string moduleVersion;              /**< Newly committed owning module version. */
    };

    /**
     * @brief Reimports one asset from durable provenance and atomically replaces its payload and sidecar.
     * @param request Absolute project/asset paths and host-owned services.
     * @param cancellation Cooperative cancellation token checked before publication.
     * @return Reimport report after durable pair and registry publication, or a typed failure.
     * @pre The caller serializes project source readers/activation through its shared host project admission.
     * @details Both native source writer leases and originally captured payload/metadata identities remain held
     * through import, expected-identity publication and compensation. Captured metadata is parsed directly;
     * rollback copies are created exclusively from the admitted bytes, never a later file generation.
     * Precommit failure restores only bytes still matching this transaction's promoted identities. Foreign
     * replacements are preserved. Reconciliation errors retain uncertainty, including post-rename directory
     * failure and derived-index persistence failure after the registry commit barrier; they do not promise
     * unchanged sources. This is not a crash-atomic filesystem pair transaction or an atomic content CAS
     * against noncooperating writes after the final native recheck. Legacy reimport uses the explicit bounded
     * 1 GiB source profile; ordinary authoring publication remains limited to 64 MiB.
     */
    [[nodiscard]] Result<AssetReimportReport> ReimportProjectAsset(const AssetReimportRequest &request,
                                                                   const CancellationToken &cancellation);
}  // namespace Horo::Assets
