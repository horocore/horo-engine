#pragma once

/**
 * @file UpdateHttpDownload.h
 * @brief Host-composed HTTPS adapter for bounded, resumable private update downloads.
 */

#include "Horo/Release/UpdateDownloadSession.h"

#include <cstdint>
#include <filesystem>
#include <functional>

namespace Horo::Release {
    /** @brief Host-selected HTTPS timeouts and optional additional certificate authority bundle. */
    struct UpdateHttpDownloadPolicy final {
        std::uint32_t connectTimeoutSeconds{15U};
        std::uint32_t requestTimeoutSeconds{3600U};
        std::filesystem::path certificateAuthorityBundle;
    };

    /** @brief Reports durable package bytes and the exact signed total on the calling worker thread. */
    using UpdateDownloadProgress = std::function<void(std::uint64_t, std::uint64_t)>;

    /** @brief Immutable inputs for one HTTPS transfer; referenced records must outlive the call. */
    struct UpdateHttpsDownloadRequest final {
        const UpdatePackageRecord &package;
        const UpdateDownloadPaths &paths;
        const UpdateDownloadLimits &limits;
        UpdateHttpDownloadPolicy policy{};
    };

    /**
     * @brief Downloads the signed package over HTTPS into private durable storage and verifies complete bytes.
     * @param request Authenticated package, private paths, resource limits, and HTTPS policy.
     * @param files Native durable filesystem kept alive for the call.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation token.
     * @param progress Optional callback invoked only after bytes and checkpoint are durable.
     * @return Verified complete-package checkpoint, or typed failure with no staged-ready marker.
     * @note The host runs this blocking adapter on a background job and keeps the private directory quiescent.
     */
    [[nodiscard]] Result<UpdateTransferCheckpoint> DownloadUpdatePackageHttps(const UpdateHttpsDownloadRequest &request,
                                                                              NativeDurableFileSystem &files,
                                                                              const Security::ArtifactVerifier &verifier,
                                                                              CancellationToken cancellation,
                                                                              const UpdateDownloadProgress &progress = {});
}  // namespace Horo::Release
