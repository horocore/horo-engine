#pragma once

/**
 * @file Sha256.h
 * @brief SHA-256 hashing and canonical digest text conversion utilities.
 */

#include "Horo/Foundation/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Horo {
    /** @brief Fixed-size binary representation of a SHA-256 digest. */
    struct Sha256Digest {
        std::array<std::uint8_t, 32> bytes{}; /**< Digest bytes in canonical big-endian order. */

        /**
         * @brief Compares digests lexicographically by their canonical byte representation.
         * @param other Digest to compare with this digest.
         * @return Ordering of this digest relative to @p other.
         */
        auto operator<=>(const Sha256Digest &other) const = default;
    };

    /**
     * @brief Computes the SHA-256 digest of an in-memory byte sequence without heap allocation.
     * @param input Bytes to hash.
     * @return SHA-256 digest of @p input.
     */
    [[nodiscard]] Sha256Digest ComputeSha256(std::span<const std::byte> input) noexcept;

    /**
     * @brief Computes SHA-256 over an ordered sequence of byte spans without concatenating them.
     * @param inputs Ordered message fragments.
     * @return SHA-256 digest of the fragments concatenated in order.
     */
    [[nodiscard]] Sha256Digest ComputeSha256Fragments(std::span<const std::span<const std::byte>> inputs) noexcept;

    /**
     * @brief Formats a digest as canonical lowercase SHA-256 text.
     * @param digest Digest to format.
     * @return Text containing `sha256:` followed by 64 lowercase hexadecimal digits.
     */
    [[nodiscard]] std::string FormatSha256(const Sha256Digest &digest);

    /**
     * @brief Parses canonical lowercase SHA-256 text.
     * @param text Text containing the exact `sha256:` prefix and 64 lowercase hexadecimal digits.
     * @return Parsed digest, or a stable typed Foundation error when @p text is noncanonical.
     */
    [[nodiscard]] Result<Sha256Digest> ParseSha256(std::string_view text);
}  // namespace Horo
