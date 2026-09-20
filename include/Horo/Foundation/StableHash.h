#pragma once

/**
 * @file StableHash.h
 * @brief Stable little-endian integer hashing for deterministic engine identities.
 */

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Horo::Foundation {
    /** @brief Small deterministic FNV-1a hash accumulator for canonical integer fields. */
    class StableHash64 final {
    public:
        static constexpr std::uint64_t OffsetBasis = 14'695'981'039'346'656'037ULL;
        static constexpr std::uint64_t Prime = 1'099'511'628'211ULL;

        /**
         * @brief Constructs an accumulator with the standard FNV-1a offset basis.
         * @param seed Initial hash value.
         */
        constexpr explicit StableHash64(const std::uint64_t seed = OffsetBasis) noexcept : value_(seed) {}

        /**
         * @brief Adds one byte to the hash stream.
         * @param value Byte to append.
         */
        constexpr void AddByte(const std::uint8_t value) noexcept {
            value_ ^= value;
            value_ *= Prime;
        }

        /**
         * @brief Adds an integral field in explicit little-endian byte order.
         * @tparam T Integral field type.
         * @param value Field to append.
         */
        template <typename T> constexpr void AddInteger(const T value) noexcept {
            static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>, "StableHash64 requires a non-boolean integer.");
            using Unsigned = std::make_unsigned_t<T>;
            const auto unsignedValue = static_cast<Unsigned>(value);
            for (std::size_t index = 0; index < sizeof(T); ++index)
                AddByte(static_cast<std::uint8_t>(unsignedValue >> (index * 8U)));
        }

        /** @brief Returns the accumulated hash value. @return Deterministic 64-bit hash. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

    private:
        std::uint64_t value_;
    };
}  // namespace Horo::Foundation
