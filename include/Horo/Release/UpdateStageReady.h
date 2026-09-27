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
}  // namespace Horo::Release
