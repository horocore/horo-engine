#pragma once

/**
 * @file UpdateTransfer.h
 * @brief Bounded update transfer and fail-closed HTTP range-resume decisions.
 */

#include "Horo/Release/UpdateManifest.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::Release {
    /** @brief Durable evidence for bytes already written to one private partial file. */
    struct UpdateTransferCheckpoint final {
        Sha256Digest packageDigest;
        std::uint64_t packageSize{};
        std::uint64_t durableBytes{};
        std::string requestedUrl;
        std::string effectiveUrl;
        std::string strongEtag;
    };

    /**
     * @brief Encodes bounded checkpoint evidence for a host-owned durable private file.
     * @param checkpoint Checkpoint produced after durable package bytes.
     * @return Exact schema-v1 bytes or an invalid-checkpoint error.
     */
    [[nodiscard]] Result<std::string> SerializeUpdateTransferCheckpoint(const UpdateTransferCheckpoint &checkpoint);

    /**
     * @brief Parses exact canonical checkpoint bytes without trusting their source.
     * @param bytes Complete checkpoint file, bounded to 4600 bytes.
     * @return Parsed evidence to revalidate against signed package and response, or a typed error.
     */
    [[nodiscard]] Result<UpdateTransferCheckpoint> ParseUpdateTransferCheckpoint(std::string_view bytes);

    /** @brief Transport-observed response headers; the caller must not infer omitted range fields. */
    struct UpdateTransferResponse final {
        unsigned status{};
        std::string requestedUrl;
        std::string effectiveUrl;
        std::string strongEtag;
        std::uint64_t contentLength{};
        std::optional<std::uint64_t> rangeStart;
        std::optional<std::uint64_t> rangeEnd;
        std::optional<std::uint64_t> rangeTotal;
    };

    /** @brief Exact offset and byte count permitted for one response body. */
    struct UpdateTransferPlan final {
        UpdateTransferCheckpoint checkpoint;
        std::uint64_t writeOffset{};
        std::uint64_t responseBytes{};
        bool resumable{};
    };

    /**
     * @brief Checks private-storage capacity before accepting package bytes.
     * @param package Authenticated package record selected by discovery.
     * @param availableBytes Capacity reported for the private stage filesystem.
     * @param maximumPackageBytes Host policy ceiling for one transfer.
     * @param reserveBytes Capacity left untouched for recovery and other work.
     * @return Success only when the complete package fits within both limits.
     */
    [[nodiscard]] Result<void> CheckUpdateTransferSpace(const UpdatePackageRecord &package, std::uint64_t availableBytes,
                                                        std::uint64_t maximumPackageBytes, std::uint64_t reserveBytes);

    /**
     * @brief Admits a complete fresh response or a validator-bound partial response.
     * @param package Authenticated package record selected by discovery.
     * @param response Transport-observed status, URL, validator, length, and range.
     * @param prior Checkpoint loaded from the private stage, if resuming.
     * @return Exact write plan or a typed rejection; never permits a silent restart over partial bytes.
     */
    [[nodiscard]] Result<UpdateTransferPlan> PlanUpdateTransfer(const UpdatePackageRecord &package, const UpdateTransferResponse &response,
                                                                const std::optional<UpdateTransferCheckpoint> &prior);

    /**
     * @brief Advances a checkpoint only after the caller has durably written body bytes.
     * @param plan Admitted response plan.
     * @param durableBodyBytes Number of response bytes known to be durable so far.
     * @return New checkpoint or rejection when the body exceeds the declared range.
     */
    [[nodiscard]] Result<UpdateTransferCheckpoint> AdvanceUpdateTransfer(const UpdateTransferPlan &plan, std::uint64_t durableBodyBytes);

    /**
     * @brief Authenticates complete private-file bytes before extraction or a ready marker.
     * @param package Authenticated package record selected by discovery.
     * @param checkpoint Durable transfer checkpoint for those bytes.
     * @param bytes Complete private-file bytes.
     * @param verifier Host-composed publisher signature verifier.
     * @return Success only for exact complete bytes, hash, and signature.
     */
    [[nodiscard]] Result<void> VerifyCompletedUpdateTransfer(const UpdatePackageRecord &package, const UpdateTransferCheckpoint &checkpoint,
                                                             std::span<const std::byte> bytes, const Security::ArtifactVerifier &verifier);

    /**
     * @brief Authenticates a complete private package file with bounded memory before extraction.
     * @param package Authenticated package record selected by discovery.
     * @param checkpoint Durable transfer checkpoint for the private file.
     * @param partialFile Quiescent private regular file kept unchanged until extraction completes.
     * @param verifier Host-composed publisher signature verifier.
     * @return Success only for exact length, SHA-256 digest, trusted publisher, and signature.
     */
    [[nodiscard]] Result<void> VerifyCompletedUpdateTransfer(const UpdatePackageRecord &package, const UpdateTransferCheckpoint &checkpoint,
                                                             const std::filesystem::path &partialFile,
                                                             const Security::ArtifactVerifier &verifier);
}  // namespace Horo::Release
