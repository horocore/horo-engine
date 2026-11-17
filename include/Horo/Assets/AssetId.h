#pragma once

/**
 * @file AssetId.h
 * @brief Stable path-independent asset and asset-type identities.
 */

#include "Horo/Foundation/Result.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Assets {
    /** @brief Stable 128-bit logical identity stored in sidecars and persistent references. */
    class AssetId final {
    public:
        AssetId() = default;
        /** @brief Parses the canonical lowercase UUID representation. @param value Text to parse. @return Parsed ID or a
         * typed identity error. */
        [[nodiscard]] static Result<AssetId> Parse(std::string_view value);

        /** @brief Constructs an identity from its persistent 16-byte representation. @param bytes Persistent UUID bytes.
         * @return Identity containing the supplied bytes; callers may use IsValid to reject the all-zero value. */
        [[nodiscard]] static constexpr AssetId FromBytes(std::array<std::uint8_t, 16> bytes) noexcept {
            return AssetId{bytes};
        }

        /** @brief Returns whether this identity is non-zero. @return True for a usable persistent identity. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the canonical lowercase UUID representation. @return Newly allocated canonical UUID text. */
        [[nodiscard]] std::string ToString() const;
        /** @brief Returns the persistent byte representation. @return Borrowed immutable bytes owned by this value. */
        [[nodiscard]] const std::array<std::uint8_t, 16> &Bytes() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const AssetId &) const noexcept = default;

    private:
        explicit constexpr AssetId(std::array<std::uint8_t, 16> bytes) noexcept : bytes_(bytes) {}

        std::array<std::uint8_t, 16> bytes_{};
    };

    /** @brief Hash adapter for AssetId containers. */
    struct AssetIdHash {
        /** @brief Hashes an asset identity. @param value Identity to hash. @return Stable process-independent hash value.
         */
        [[nodiscard]] std::size_t operator()(const AssetId &value) const noexcept;
    };

    /** @brief Validated stable asset type identifier such as core.mesh. */
    class AssetTypeId final {
    public:
        /** @brief Creates an empty identifier. @throws std::bad_alloc When checked-library storage cannot be allocated. */
        AssetTypeId() = default;
        /** @brief Copies the owned identifier. @param other Identifier to copy. @throws std::bad_alloc When storage is unavailable. */
        AssetTypeId(const AssetTypeId &other) = default;
        /** @brief Copies the owned identifier. @param other Identifier to copy. @return This identifier.
         * @throws std::bad_alloc When storage is unavailable. */
        AssetTypeId &operator=(const AssetTypeId &other) = default;
        /** @brief Transfers the identifier after preparing empty destination storage. @param other Identifier to transfer.
         * @post On allocation failure other is unchanged; on success other is empty.
         * @throws std::bad_alloc When checked-library storage cannot be allocated. */
        AssetTypeId(AssetTypeId &&other);
        /** @brief Transfers an identifier into existing destination storage. @param other Identifier to transfer.
         * @return This identifier. */
        AssetTypeId &operator=(AssetTypeId &&other) noexcept = default;
        /** @brief Parses a canonical lowercase dotted identifier. @param value Text to validate. @return Validated type ID
         * or a typed format error. @throws std::bad_alloc When owned identifier storage cannot be allocated. */
        [[nodiscard]] static Result<AssetTypeId> Parse(std::string_view value);
        /** @brief Returns the canonical identifier text. @return Borrowed text owned by this value. */
        [[nodiscard]] const std::string &Value() const noexcept;
        [[nodiscard]] auto operator<=>(const AssetTypeId &) const noexcept = default;

    private:
        explicit AssetTypeId(std::string_view value) : value_(value) {}

        std::string value_ = std::string(std::size_t{0}, char{});
    };
}  // namespace Horo::Assets
