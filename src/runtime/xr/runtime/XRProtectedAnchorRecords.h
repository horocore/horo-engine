#pragma once

/** @file XRProtectedAnchorRecords.h
 * @brief Private canonical Save wire format for local protected-store lookups.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/XR/XRErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::XR::AnchorPersistenceDetail {
    /** @brief Canonical v1 envelope has exactly one protected lookup and no live session data. */
    inline constexpr std::array Magic{std::byte{'X'}, std::byte{'R'}, std::byte{'A'}, std::byte{'1'}};
    inline constexpr std::size_t IdBytes = 16;
    inline constexpr std::size_t RecordBytes = Magic.size() + 3 * IdBytes + 4 + 8 + 8;
    using IdentityBytes = std::array<std::byte, IdBytes>;

    /** @brief Save owns these durable reference fields. Provider handles, tokens and poses stay outside the record. */
    struct ProtectedAnchorRecord final {
        IdentityBytes provider;
        IdentityBytes localPartition;
        IdentityBytes lookup;
        std::uint32_t providerVersion{};
        std::uint64_t accessRevision{};
        std::uint64_t expiresAt{};

        constexpr auto operator<=>(const ProtectedAnchorRecord &) const noexcept = default;
    };

    /** @brief An all-zero identity is reserved for unset or malformed input. */
    [[nodiscard]] constexpr bool HasIdentity(const IdentityBytes &value) noexcept {
        return std::ranges::any_of(value, [](const std::byte octet) {
            return octet != std::byte{};
        });
    }

    /** @brief Checks canonical field representation, independently of current host admission. */
    [[nodiscard]] constexpr bool Valid(const ProtectedAnchorRecord &record) noexcept {
        return HasIdentity(record.provider) && HasIdentity(record.localPartition) && HasIdentity(record.lookup) &&
               record.providerVersion != 0 && record.accessRevision != 0 && record.expiresAt != 0;
    }

    /** @brief Emits one scalar in the stable little-endian Save encoding. */
    template <typename T> constexpr void Write(std::byte *&cursor, T value) noexcept {
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            *cursor++ = static_cast<std::byte>(value & 0xffU);
            value >>= 8U;
        }
    }

    /** @brief Reads one scalar from a previously validated complete fixed-size envelope. */
    template <typename T> [[nodiscard]] constexpr T Read(const std::byte *&cursor) noexcept {
        T value{};
        for (std::size_t index = 0; index < sizeof(T); ++index)
            value |= std::to_integer<T>(*cursor++) << (index * 8U);
        return value;
    }

    /** @brief Encodes a validated record without touching the provider or creating a dynamic buffer. */
    [[nodiscard]] constexpr std::array<std::byte, RecordBytes> Encode(const ProtectedAnchorRecord &record) noexcept {
        std::array<std::byte, RecordBytes> bytes{};
        auto *cursor = std::ranges::copy(Magic, bytes.data()).out;
        cursor = std::ranges::copy(record.provider, cursor).out;
        cursor = std::ranges::copy(record.localPartition, cursor).out;
        cursor = std::ranges::copy(record.lookup, cursor).out;
        Write(cursor, record.providerVersion);
        Write(cursor, record.accessRevision);
        Write(cursor, record.expiresAt);
        return bytes;
    }

    /** @brief Rejects unknown versions, truncation and trailing data before materializing a Save candidate. */
    [[nodiscard]] inline Result<ProtectedAnchorRecord> Decode(const std::span<const std::byte> bytes) {
        using Outcome = Result<ProtectedAnchorRecord>;
        if (bytes.size() != RecordBytes || !std::ranges::equal(bytes.first(Magic.size()), Magic))
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        ProtectedAnchorRecord record{};
        const auto *cursor = bytes.data() + Magic.size();
        std::copy_n(cursor, IdBytes, record.provider.begin());
        cursor += IdBytes;
        std::copy_n(cursor, IdBytes, record.localPartition.begin());
        cursor += IdBytes;
        std::copy_n(cursor, IdBytes, record.lookup.begin());
        cursor += IdBytes;
        record.providerVersion = Read<std::uint32_t>(cursor);
        record.accessRevision = Read<std::uint64_t>(cursor);
        record.expiresAt = Read<std::uint64_t>(cursor);
        return Valid(record) ? Outcome::Success(record) : Outcome::Failure(MakeError(XRErrors::OperationInvalid));
    }
}  // namespace Horo::XR::AnchorPersistenceDetail
