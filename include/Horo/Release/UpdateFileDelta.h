#pragma once

/**
 * @file UpdateFileDelta.h
 * @brief Exact file-level delta planning against authenticated release inventories.
 */

#include "Horo/Release/UpdateStagedTree.h"

#include <span>
#include <vector>

namespace Horo::Release {
    /** @brief Source of one file in a fully reconstructed target stage. */
    enum class UpdateDeltaFileSource : std::uint8_t {
        VerifiedBase,
        VerifiedDelta
    };

    /** @brief One target file and the verified staging tree from which to copy it. */
    struct UpdateDeltaFile final {
        UpdateStagedFile target;
        UpdateDeltaFileSource source{UpdateDeltaFileSource::VerifiedBase};
    };

    /** @brief Complete deterministic reconstruction plan; deleted base files are absent from targetFiles. */
    struct UpdateFileDeltaPlan final {
        Sha256Digest baseInventoryDigest;
        Sha256Digest targetInventoryDigest;
        Sha256Digest deltaInventoryDigest;
        std::vector<UpdateStagedFile> baseFiles;
        std::vector<UpdateStagedFile> deltaFiles;
        std::vector<UpdateDeltaFile> targetFiles;
    };

    /**
     * @brief Compares exact base, delta, and target inventories before any staging mutation.
     * @param baseFiles Complete file inventory from an authenticated base package.
     * @param targetFiles Complete file inventory authorized for the candidate release.
     * @param deltaFiles Exact changed/new files plus the required product entrypoint from an authenticated delta ZIP.
     * @param expectedBaseDigest Authenticated digest of the canonical base inventory.
     * @param expectedTargetDigest Authenticated digest of the canonical full target inventory.
     * @param expectedDeltaDigest Authenticated digest of the canonical delta package inventory.
     * @param limits Host archive limits applied to every inventory.
     * @return Plan only when every changed file and the entrypoint are supplied exactly once and both full-manifest identities match.
     * @note The caller authenticates package signatures and source metadata before supplying these inventories.
     *       The final stage must be verified with VerifyUpdateStagedTree against targetFiles before publication.
     */
    [[nodiscard]] Result<UpdateFileDeltaPlan> PlanUpdateFileDelta(
        std::span<const UpdateStagedFile> baseFiles, std::span<const UpdateStagedFile> targetFiles,
        std::span<const UpdateStagedFile> deltaFiles, const Sha256Digest &expectedBaseDigest, const Sha256Digest &expectedTargetDigest,
        const Sha256Digest &expectedDeltaDigest, const UpdateArchiveLimits &limits);

    /**
     * @brief Verifies a private reconstructed tree against the plan's complete target inventory.
     * @param stageRoot Quiescent private stage assembled from verified base and delta sources.
     * @param plan Plan bound to authenticated base, delta, and target inventory digests.
     * @param limits Host archive limits.
     * @return Success only when every target byte matches and no extra file or link exists.
     */
    [[nodiscard]] Result<void> VerifyUpdateFileDeltaStage(const std::filesystem::path &stageRoot, const UpdateFileDeltaPlan &plan,
                                                          const UpdateArchiveLimits &limits);
}  // namespace Horo::Release
