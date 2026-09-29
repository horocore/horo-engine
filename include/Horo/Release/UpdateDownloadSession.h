#pragma once

/**
 * @file UpdateDownloadSession.h
 * @brief Host-composed private update download session with durable chunk checkpoints.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Release/UpdateTransfer.h"

#include <filesystem>
#include <span>

namespace Horo::Release {
    /** @brief Two host-owned paths inside one protected private staging directory. */
    struct UpdateDownloadPaths final {
        std::filesystem::path partialFile;
        std::filesystem::path checkpointFile;
    };

    /** @brief Admission limits for one private package transfer. */
    struct UpdateDownloadLimits final {
        std::uint64_t maximumPackageBytes{};
        std::uint64_t reserveBytes{};
    };

    /**
     * @brief One single-threaded HTTP response body admitted against a signed package and durable checkpoint.
     * @note The host owns the private directory and keeps it quiescent except for this session.
     */
    class UpdateDownloadSession final {
    public:
        /**
         * @brief Loads recovery evidence, preflights remaining space, and admits exact response headers before body bytes.
         * @param package Authenticated package selected by update discovery.
         * @param response Transport-observed final HTTP status, URL, validator, length, and range.
         * @param paths Private partial and checkpoint paths in the same protected directory.
         * @param limits Host policy size ceiling and free-space reserve.
         * @param files Host-composed native durable filesystem, which outlives the session.
         * @param cancellation Parent operation cancellation token.
         * @return Session ready to receive bounded chunks, or typed rejection without changing files.
         */
        [[nodiscard]] static Result<UpdateDownloadSession> Begin(const UpdatePackageRecord &package, const UpdateTransferResponse &response,
                                                                 UpdateDownloadPaths paths, const UpdateDownloadLimits &limits,
                                                                 NativeDurableFileSystem &files, CancellationToken cancellation);

        /**
         * @brief Appends one body block and publishes its checkpoint only after the bytes are durable.
         * @param bytes Nonempty response body block; the complete response may span many calls.
         * @return Durable cumulative package progress, or a typed failure that stops this session.
         */
        [[nodiscard]] Result<UpdateTransferCheckpoint> Append(std::span<const std::byte> bytes);

        /**
         * @brief Closes an exact response body and authenticates the package when it is complete.
         * @param verifier Host-composed publisher signature verifier.
         * @return Last durable checkpoint; complete packages have passed length, hash, and signature verification.
         */
        [[nodiscard]] Result<UpdateTransferCheckpoint> Finish(const Security::ArtifactVerifier &verifier);

    private:
        UpdateDownloadSession(UpdatePackageRecord package, UpdateTransferPlan plan, UpdateDownloadPaths paths,
                              NativeDurableFileSystem &files, CancellationToken cancellation);

        UpdatePackageRecord package_;
        UpdateTransferPlan plan_;
        UpdateDownloadPaths paths_;
        NativeDurableFileSystem *files_;
        CancellationToken cancellation_;
        std::uint64_t receivedBytes_{};
        bool failed_{};
        bool finished_{};
    };
}  // namespace Horo::Release
