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

    /** @brief Incremental SHA-256 state for bounded-memory hashing of large artifact files. */
    class Sha256Builder final {
    public:
        /**
         * @brief Appends bytes to the hash before finalization.
         * @param input Next contiguous byte fragment.
         * @return False if already finalized or the SHA-256 bit-length limit would be exceeded.
         */
        [[nodiscard]] bool Update(std::span<const std::byte> input) noexcept;

        /**
         * @brief Seals the hash; repeated calls return the same digest.
         * @return SHA-256 digest of all accepted fragments.
         */
        [[nodiscard]] Sha256Digest Finalize() noexcept;

    private:
        std::array<std::uint32_t, 8> state_{
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
        };
        std::array<std::uint8_t, 64> block_{};
        std::size_t blockBytes_{};
        std::uint64_t totalBytes_{};
        bool finalized_{};
        Sha256Digest digest_{};
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
