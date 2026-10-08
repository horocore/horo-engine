#pragma once

/**
 * @file AssetCook.h
 * @brief Backend-neutral asset cooking values and artifact envelope.
 */

#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/AssetCookTargetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    using ::Horo::AssetCookTargetId;

    /** @brief Bounded limits applied to cook operations and artifact sizes. */
    struct AssetCookLimits {
        std::size_t maximumSourceBytes{256U * 1024U * 1024U};   /**< Maximum source bytes read by a cooker. */
        std::size_t maximumArtifactBytes{256U * 1024U * 1024U}; /**< Maximum encoded artifact bytes. */
        std::size_t maximumAssets{16U * 1024U};                 /**< Maximum assets in one cook operation. */
        std::size_t maximumConcurrentCooks{8};                  /**< Maximum concurrent cook jobs. */
    };

    /** @brief Versioned cooked artifact envelope carrying identity, digests, and payload. */
    struct AssetCookArtifact {
        static constexpr std::uint32_t CurrentFormatVersion = 1; /**< Binary envelope format version. */

        AssetId id;                        /**< Stable asset identity. */
        AssetTypeId type;                  /**< Asset type identity. */
        AssetCookTargetId target;          /**< Cook target identity. */
        Sha256Digest cacheKeyDigest;       /**< Full CacheKeyV1 digest, host-computed. */
        Sha256Digest sourceDigest;         /**< SHA-256 of the source bytes. */
        Sha256Digest payloadDigest;        /**< SHA-256 of the payload bytes. */
        std::vector<std::uint8_t> payload; /**< Cooked payload bytes. */
    };

    /**
     * @brief Encodes a cooked artifact into its versioned binary envelope.
     * @param artifact Artifact to encode.
     * @param limits Size bounds checked before encoding.
     * @return Encoded bytes or a typed too-large error. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeCookedArtifact(const AssetCookArtifact &artifact,
                                                                         const AssetCookLimits &limits = {});

    /**
     * @brief Decodes and verifies a versioned binary artifact envelope.
     * @param bytes Encoded artifact bytes.
     * @param limits Size bounds checked during decoding.
     * @return Verified artifact or a typed malformed/hash-mismatch/unsupported error. */
    [[nodiscard]] Result<AssetCookArtifact> DecodeCookedArtifact(std::span<const std::uint8_t> bytes, const AssetCookLimits &limits = {});
    /** @brief Decodes an immutable byte lease without aliasing it as an integer buffer.
     * @param bytes Encoded artifact bytes. @param limits Size bounds checked during decoding.
     * @return Verified artifact or a typed malformed/hash-mismatch/unsupported error.
     */
    [[nodiscard]] Result<AssetCookArtifact> DecodeCookedArtifactBytes(std::span<const std::byte> bytes, const AssetCookLimits &limits = {});
}  // namespace Horo::Assets
