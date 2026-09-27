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

    /**
     * @brief Downloads the signed package over HTTPS into private durable storage and verifies complete bytes.
     * @param package Authenticated update package selected by discovery.
     * @param paths Private partial and checkpoint files owned by the host.
     * @param limits Host package-size and free-space-reserve policy.
     * @param files Native durable filesystem kept alive for the call.
     * @param verifier Trusted publisher signature verifier.
     * @param cancellation Cooperative cancellation token.
     * @param policy HTTPS timeouts and optional additional CA trust bundle; TLS verification is always enabled.
     * @param progress Optional callback invoked only after bytes and checkpoint are durable.
     * @return Verified complete-package checkpoint, or typed failure with no staged-ready marker.
     * @note The host runs this blocking adapter on a background job and keeps the private directory quiescent.
     */
    [[nodiscard]] Result<UpdateTransferCheckpoint> DownloadUpdatePackageHttps(
        const UpdatePackageRecord &package, UpdateDownloadPaths paths, const UpdateDownloadLimits &limits, NativeDurableFileSystem &files,
        const Security::ArtifactVerifier &verifier, CancellationToken cancellation, const UpdateHttpDownloadPolicy &policy = {},
        const UpdateDownloadProgress &progress = {});
}  // namespace Horo::Release
