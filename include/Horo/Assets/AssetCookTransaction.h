#pragma once

/** @file AssetCookTransaction.h
 * @brief Locked, durable replacement of one artifact within a complete cooked generation.
 */
#include "Horo/Assets/AssetCookOutput.h"

namespace Horo::Assets {
    /**
     * @brief Recovers and pins the current base under the common writer lock, preserves unrelated artifacts and publishes one replacement.
     * @param root Absolute canonical target output root; all writers honor .cook-writer.lock.
     * @param target Exact cooked target.
     * @param entry Replacement identity, canonical <AssetId>.cooked filename and actual envelope digest.
     * @param artifact Complete encoded and validated replacement envelope.
     * @param maximumBytes Aggregate generation allocation ceiling.
     * @param limits Artifact/count bounds.
     * @param policy Required durable writer, optional bounded contention/checkpoint policy and final adoption check.
     * @return Published generation or typed lock/base/capacity/freshness/I/O failure.
     * @post Carried artifact bytes/hashes are unchanged; new generation filenames are canonical asset-ID names.
     * Old pinned generations retain their original filenames.
     * @post Failure before atomic replacement preserves current.json; inactive staging never selects a generation.
     * @post The native writer lease remains held through the final callback, atomic pointer commit and transaction return.
     * @details Missing current.json is bootstrap only when no immutable generation exists. The final callback may acquire a source
     *          adoption guard retained by its host through return. A value with durabilityError must still be adopted as committed.
     */
    [[nodiscard]] Result<AssetCookGeneration> PublishCookArtifactReplacement(const std::filesystem::path &root,
                                                                             const AssetCookTargetId &target, AssetCookManifestEntry entry,
                                                                             std::vector<std::uint8_t> artifact, std::size_t maximumBytes,
                                                                             const AssetCookLimits &limits,
                                                                             const AssetCookPublicationPolicy &policy);
}  // namespace Horo::Assets
