#pragma once

/**
 * @file UpdateZipStagingJob.h
 * @brief Host worker operation from HTTPS update download to verified ZIP stage.
 */

#include "Horo/Release/UpdateHttpDownload.h"
#include "Horo/Release/UpdateStageReady.h"

namespace Horo::Release {
    /**
     * @brief Downloads or resumes a signed ZIP, then publishes an exact private stage.
     * @param package Authenticated ZIP package selected by update discovery.
     * @param paths Private package and checkpoint paths in one protected directory.
     * @param stageRoot Absent sibling directory for extracted installation files.
     * @param downloadLimits Host policy for package size and storage reserve.
     * @param archiveLimits Host policy for archive entries and expanded bytes.
     * @param files Native durable filesystem kept alive for the whole operation.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation token.
     * @param policy HTTPS timeouts and optional additional CA bundle.
     * @param progress Optional callback after durable download progress.
     * @return Durable ready marker, or failure without publishing one.
     * @note The host dispatches this blocking operation on a worker and keeps its
     * private directory quiescent. A complete durable checkpoint skips the network
     * while still rechecking the signed package before extraction.
     */
    [[nodiscard]] Result<std::filesystem::path> PrepareZipUpdateStageHttps(
        const UpdatePackageRecord &package, UpdateDownloadPaths paths, const std::filesystem::path &stageRoot,
        const UpdateDownloadLimits &downloadLimits, const UpdateArchiveLimits &archiveLimits, NativeDurableFileSystem &files,
        const Security::ArtifactVerifier &verifier, CancellationToken cancellation, const UpdateHttpDownloadPolicy &policy = {},
        const UpdateDownloadProgress &progress = {});
}  // namespace Horo::Release
