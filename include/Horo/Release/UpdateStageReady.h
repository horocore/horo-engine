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
#include <vector>

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

    /** @brief Signed Linux tar.gz and private stage inputs for one extraction transaction. */
    struct VerifiedTarGzipUpdateRequest final {
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
     * @brief Reauthenticates a ready stage and its exact marker immediately before activation.
     * @param package Signed package selected by update discovery.
     * @param checkpoint Complete durable checkpoint for the quiescent package file.
     * @param packageFile Complete private package file beside stageRoot.
     * @param stageRoot Quiescent staged tree whose sibling marker has already been published.
     * @param inventory Complete file inventory authenticated from the package.
     * @param limits Host archive limits used when the stage was created.
     * @param verifier Trusted publisher signature verifier.
     * @return Success only when marker, package, and staged file bytes agree.
     * @note The activation host holds the private parent quiescent throughout verification and switching.
     */
    [[nodiscard]] Result<void> VerifyReadyUpdateStage(const UpdatePackageRecord &package, const UpdateTransferCheckpoint &checkpoint,
                                                      const std::filesystem::path &packageFile, const std::filesystem::path &stageRoot,
                                                      std::span<const UpdateStagedFile> inventory, const UpdateArchiveLimits &limits,
                                                      const Security::ArtifactVerifier &verifier);

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

    /**
     * @brief Authenticates and indexes a canonical gzip-wrapped ustar package without extracting it.
     * @param package Signed Linux tar.gz package record.
     * @param checkpoint Complete durable checkpoint for packageFile.
     * @param packageFile Private, quiescent package file.
     * @param limits Maximum file count, file size, and expanded payload bytes.
     * @param verifier Trusted publisher signature verifier.
     * @return Validated regular-file/directory index or a typed archive failure.
     * @note This is a preflight reader. A separate staging transaction must verify the internal file inventory and extract bytes.
     */
    [[nodiscard]] Result<std::vector<UpdateArchiveEntry>> IndexVerifiedTarGzipPackage(const UpdatePackageRecord &package,
                                                                                      const UpdateTransferCheckpoint &checkpoint,
                                                                                      const std::filesystem::path &packageFile,
                                                                                      const UpdateArchiveLimits &limits,
                                                                                      const Security::ArtifactVerifier &verifier);

    /**
     * @brief Authenticates, indexes, extracts, and publishes a ready Linux tar.gz stage.
     * @param request Signed package, exact private paths, and expansion limits.
     * @param files Native durable filesystem kept alive for the transaction.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation before ready publication.
     * @return Ready-marker path only after every extracted file matches the authenticated internal inventory.
     * @note The host keeps the package and private parent quiescent throughout this call.
     */
    [[nodiscard]] Result<std::filesystem::path> StageVerifiedTarGzipUpdate(const VerifiedTarGzipUpdateRequest &request,
                                                                           NativeDurableFileSystem &files,
                                                                           const Security::ArtifactVerifier &verifier,
                                                                           CancellationToken cancellation);
    /**
     * @brief Authenticates and extracts a delta ZIP into a new private tree without publishing a ready marker.
     * @param request Signed delta ZIP, complete checkpoint, private paths, and archive limits.
     * @param files Native durable filesystem held alive for this call.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation during indexing and extraction.
     * @return Exact extracted inventory or failure with the incomplete tree removed.
     * @note A delta tree is not a complete installation and must never be activated directly.
     */
    [[nodiscard]] Result<std::vector<UpdateStagedFile>> StageVerifiedDeltaZipUpdate(const VerifiedZipUpdateRequest &request,
                                                                                    NativeDurableFileSystem &files,
                                                                                    const Security::ArtifactVerifier &verifier,
                                                                                    CancellationToken cancellation);
}  // namespace Horo::Release
