#pragma once

/**
 * @file UpdateTarGzipStagingJob.h
 * @brief Host worker operation from HTTPS update download to verified Linux tar.gz stage.
 */

#include "Horo/Release/UpdateHttpDownload.h"
#include "Horo/Release/UpdateStageReady.h"

namespace Horo::Release {
    /** @brief Immutable paths and policies for one HTTPS-to-tar.gz staging operation. */
    struct UpdateTarGzipStagingRequest final {
        const UpdatePackageRecord &package;
        const UpdateDownloadPaths &paths;
        const std::filesystem::path &stageRoot;
        const UpdateDownloadLimits &downloadLimits;
        const UpdateArchiveLimits &archiveLimits;
        UpdateHttpDownloadPolicy policy{};
    };

    /**
     * @brief Downloads or resumes a signed Linux tar.gz package, then publishes an exact private stage.
     * @param request Authenticated package, private paths, absent stage, and resource policies.
     * @param files Native durable filesystem kept alive for the whole operation.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation token.
     * @param progress Optional callback after durable download progress.
     * @return Durable ready marker, or failure without publishing one.
     * @note The host dispatches this blocking operation on a worker and keeps its
     * private directory quiescent. A complete durable checkpoint skips the network
     * while still rechecking the signed package before extraction.
     */
    [[nodiscard]] Result<std::filesystem::path> PrepareTarGzipUpdateStageHttps(const UpdateTarGzipStagingRequest &request,
                                                                               NativeDurableFileSystem &files,
                                                                               const Security::ArtifactVerifier &verifier,
                                                                               CancellationToken cancellation,
                                                                               const UpdateDownloadProgress &progress = {});
}  // namespace Horo::Release
