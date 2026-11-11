#pragma once
/** @file ColorOutputPackageInternal.h @brief Private bounded data decoder helpers, never process/cook activation. */
#include "ColorOutputPackage.h"
#include "ColorOutputShaderRecipe.h"
#include "JsonUtils.h"

#include <stdexcept>
#include <utility>

namespace Horo::Render::Internal::ColorPackageDetail {
    using Bytes = std::vector<std::uint8_t>;
    using Foundation::Json;

    /** @brief Preserves original typed failures across the synchronous cold decoder boundary. */
    struct Failure final {
        Error error;
    };

    /** @brief Preserves one actual failing owner result without manufacturing success metadata. */
    template <class T> T Take(Result<T> result) {
        if (result.HasError())
            throw Failure{result.ErrorValue()};
        return std::move(result).Value();
    }

    /** @brief Preserves a failing synchronous validation result. @param result Actual owner result. */
    inline void Take(const Result<void> &result) {
        if (result.HasError())
            throw Failure{result.ErrorValue()};
    }

    /** @brief Rejects inconsistent cold data before publishing anything. @param valid Required predicate. */
    void Require(bool valid);
    /** @brief Creates the product's bounded malformed-input failure. @return Typed shader-tool output failure. */
    [[nodiscard]] Error Invalid();
    /** @brief Validates an absolute ordinary source root. @param root Explicit directory. @return Canonical actual root. */
    [[nodiscard]] std::filesystem::path Root(const std::filesystem::path &root);
    /** @brief Reads one exact child while rejecting symlinks/escapes. @param root Canonical directory.
     * @param relative Fixed relative file. @param maximum Finite byte limit. @return Actual bytes or preserved failure.
     */
    [[nodiscard]] Bytes Read(const std::filesystem::path &root, std::string_view relative, std::size_t maximum);
    /** @brief Parses finite duplicate-rejecting JSON. @param bytes Actual bounded file. @return Parsed object or failure. */
    [[nodiscard]] Json Parse(const Bytes &bytes);
    /** @brief Requires exact portable raw-hex SHA identity. @param value Actual digest string. @return Digest or failure. */
    [[nodiscard]] Sha256Digest Digest(const Json &value);
    /** @brief Requires a checked uint32 value. @param value Actual JSON integer. @return Exact integer, never truncation. */
    [[nodiscard]] std::uint32_t UInt(const Json &value);
    /** @brief Hashes exact original bytes. @param bytes Actual payload. @return SHA256 identity. */
    [[nodiscard]] Sha256Digest Hash(const Bytes &bytes) noexcept;
    /** @brief Serializes canonical finite identity JSON. @param value Generation data. @return Exact sorted JSON bytes plus newline. */
    [[nodiscard]] Bytes Encode(const Json &value);
    /** @brief Validates one complete producer generation already read under an owned exact root.
     * @param files Seven actual producer files. @return Immutable validated program.
     */
    [[nodiscard]] std::shared_ptr<const Render::CookedColorOutputProgram> DecodeAnalytic(const std::array<Bytes, 7> &files);

}  // namespace Horo::Render::Internal::ColorPackageDetail
