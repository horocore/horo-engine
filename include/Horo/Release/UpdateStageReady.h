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
    /** @brief Verified package and tree inputs for one durable ready-marker publication. */
    struct VerifiedUpdateStageRequest final {
        const UpdatePackageRecord &package;
        const UpdateTransferCheckpoint &checkpoint;
        const std::filesystem::path &packageFile;
        const std::filesystem::path &stageRoot;
        std::span<const UpdateStagedFile> inventory;
        const UpdateArchiveLimits &limits;
    };

    /** @brief Signed ZIP and private destination inputs for one extraction transaction. */
    struct VerifiedZipUpdateRequest final {
        const UpdatePackageRecord &package;
        const UpdateTransferCheckpoint &checkpoint;
        const std::filesystem::path &packageFile;
        const std::filesystem::path &stageRoot;
        const UpdateArchiveLimits &limits;
    };

    /**
     * @brief Publishes a version-private ready marker only after package and staged-tree authentication.
     * @param request Signed package, durable checkpoint, private paths, inventory, and limits.
     * @param files Native durable filesystem held alive for this call.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation before ready publication.
     * @return Path to the atomically published sibling marker, or failure without a ready marker.
     * @note The host holds package, stage, and their parent private and quiescent for the entire call.
     */
    [[nodiscard]] Result<std::filesystem::path> PublishVerifiedUpdateStage(const VerifiedUpdateStageRequest &request,
                                                                           NativeDurableFileSystem &files,
                                                                           const Security::ArtifactVerifier &verifier,
                                                                           CancellationToken cancellation);

    /**
     * @brief Authenticates a ZIP and its internal file inventory, extracts it into a new private stage, then publishes ready.
     * @param request Signed ZIP, durable checkpoint, private paths, and archive limits.
     * @param files Native durable filesystem held alive for this call.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation during indexing and extraction.
     * @return Atomically published ready marker path, or failure with the incomplete stage removed.
     * @note The host keeps the package and private parent quiescent throughout this call.
     */
    [[nodiscard]] Result<std::filesystem::path> StageVerifiedZipUpdate(const VerifiedZipUpdateRequest &request,
                                                                       NativeDurableFileSystem &files,
                                                                       const Security::ArtifactVerifier &verifier,
                                                                       CancellationToken cancellation);
}  // namespace Horo::Release
