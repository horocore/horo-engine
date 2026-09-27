#pragma once

/**
 * @file UpdateStageReady.h
 * @brief Durable ready publication after complete package and private tree verification.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Release/UpdateStagedTree.h"
#include "Horo/Release/UpdateTransfer.h"

#include <filesystem>
#include <span>

namespace Horo::Release {
    /**
     * @brief Publishes a version-private ready marker only after package and staged-tree authentication.
     * @param package Signed package identity selected by update discovery.
     * @param checkpoint Durable checkpoint for the quiescent complete private package file.
     * @param packageFile Quiescent complete private package file outside the staged tree.
     * @param stageRoot Quiescent private extraction root containing only declared files and their parents.
     * @param inventory Complete per-file inventory read from that authenticated package.
     * @param limits Host entry and expanded-size limits.
     * @param files Native durable filesystem held alive for this call.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation before ready publication.
     * @return Path to the atomically published sibling marker, or failure without a ready marker.
     * @note The host holds package, stage, and their parent private and quiescent for the entire call.
     */
    [[nodiscard]] Result<std::filesystem::path> PublishVerifiedUpdateStage(
        const UpdatePackageRecord &package, const UpdateTransferCheckpoint &checkpoint, const std::filesystem::path &packageFile,
        const std::filesystem::path &stageRoot, std::span<const UpdateStagedFile> inventory, const UpdateArchiveLimits &limits,
        NativeDurableFileSystem &files, const Security::ArtifactVerifier &verifier, CancellationToken cancellation);

    /**
     * @brief Authenticates a ZIP and its internal file inventory, extracts it into a new private stage, then publishes ready.
     * @param package Signed ZIP package identity selected by update discovery.
     * @param checkpoint Durable checkpoint for the complete quiescent package file.
     * @param packageFile Complete private package file outside the stage.
     * @param stageRoot Absent stage directory below the same protected parent as packageFile.
     * @param limits Maximum entry count, per-file bytes, and total expanded bytes.
     * @param files Native durable filesystem held alive for this call.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation during indexing and extraction.
     * @return Atomically published ready marker path, or failure with the incomplete stage removed.
     * @note The host keeps the package and private parent quiescent throughout this call.
     */
    [[nodiscard]] Result<std::filesystem::path> StageVerifiedZipUpdate(
        const UpdatePackageRecord &package, const UpdateTransferCheckpoint &checkpoint, const std::filesystem::path &packageFile,
        const std::filesystem::path &stageRoot, const UpdateArchiveLimits &limits, NativeDurableFileSystem &files,
        const Security::ArtifactVerifier &verifier, CancellationToken cancellation);
}  // namespace Horo::Release
