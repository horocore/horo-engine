#pragma once

/** @file FoliageClusterCookInternal.h
 * @brief Target-private shared integrity primitives for detached preparation and owner publication.
 */
#include "Horo/Terrain/FoliageClusterCook.h"

namespace Horo::Terrain::FoliageClusterCookInternal {
    /** @brief Hashes every captured profile field. @param profile Exact immutable target policy. @return Canonical profile identity. */
    [[nodiscard]] Sha256Digest ProfileFingerprint(const FoliageClusterCookProfile &profile);

    /**
     * @brief Hashes exact manifest membership without allocating or retaining references.
     * @param dataset Stable dataset identity.
     * @param content Aggregate publication revision.
     * @param fingerprint Complete source/profile identity.
     * @param clusters Canonically ordered owned clusters.
     * @return Canonical manifest integrity.
     */
    [[nodiscard]] Sha256Digest ManifestFingerprint(TerrainDatasetId dataset, TerrainContentRevision content,
                                                   const Sha256Digest &fingerprint, std::span<const CookedFoliageCluster> clusters);

    /**
     * @brief Verifies neutral metadata, typed records, bounds and encoded digest without repairing evidence.
     * @param cluster Cook-issued cluster to recheck.
     * @param dataset Expected dataset.
     * @param capability Expected exact capability revision.
     * @param profile Expected profile identity.
     * @param cancellation Cooperative observer checked during record and hash visits.
     * @return True for complete consistent evidence; false for damage or cancellation.
     */
    [[nodiscard]] bool ValidateCluster(const CookedFoliageCluster &cluster, TerrainDatasetId dataset, TerrainCapabilityRevision capability,
                                       const Sha256Digest &profile, const CancellationToken &cancellation);
}  // namespace Horo::Terrain::FoliageClusterCookInternal
