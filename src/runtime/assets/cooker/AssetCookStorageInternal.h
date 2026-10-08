/**
 * @file AssetCookStorageInternal.h
 * @brief Target-private contracts shared by cook publication, verified reads and locked recovery.
 */
#pragma once

#include "Horo/Assets/AssetCookOutput.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets::CookStorageDetail {
    inline constexpr std::size_t MaximumGenerationBytes = 1024U * 1024U * 1024U; /**< Hard aggregate encoded generation ceiling. */
    inline constexpr std::size_t MaximumSelectorBytes = MaximumAssetCookTargetIdBytes + 512U; /**< Bounded sole-selector envelope. */

    /** @brief Caller limits may tighten built-in ceilings without expanding parsing or recovery work. */
    bool AdmittedLimits(const AssetCookLimits &limits);

    /** @brief Formats a SHA-256 digest as a canonical lowercase hexadecimal string. */
    [[nodiscard]] std::string HexEncodeSha256(const Sha256Digest &digest);

    /** @brief Produces canonical manifest JSON from a sorted, verified inventory. */
    std::string BuildManifestJson(std::string_view target, std::span<const AssetCookManifestEntry> entries);

    /** @brief Rejects nonportable, escaping, oversized and Windows-reserved artifact filenames. */
    [[nodiscard]] bool IsSafeArtifactFile(const std::string_view file) noexcept;

    /** @brief Confirms that absolute, canonical path components keep the candidate below its root. */
    [[nodiscard]] bool IsSafePathWithin(const std::filesystem::path &base, const std::filesystem::path &candidate) noexcept;

    /** @brief Rejects links and lexical escapes in every existing path component, including ancestors. */
    [[nodiscard]] bool HasPlainPath(const std::filesystem::path &path);

    /** @brief Confirms that a file is regular, single-link and below a plain path. */
    [[nodiscard]] bool IsPlainFile(const std::filesystem::path &path);

    /** @brief Reads one plain file within the caller's encoded-byte bound. */
    Result<std::vector<std::uint8_t>> ReadFile(const std::filesystem::path &path, std::size_t maxBytes);

    /** @brief Replaces prepared content and uses a native receipt to distinguish a committed selector with unknown durability. */
    Result<void> ReplaceDurably(DurableFileSystem *files, const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                std::optional<Error> *postCommitError);

    /** @brief Writes one new operation-owned file without overwriting or adopting existing content. */
    Result<void> WritePrivate(const std::filesystem::path &path, const std::span<const std::uint8_t> bytes, DurableFileSystem *files);

    /** @brief Encodes the sole selector's explicit unpublished state without selecting an active generation. */
    std::string UnpublishedSelector(const AssetCookTargetId &target);

    /** @brief Verifies bytes against a manifest hash and typed asset identity for the requested target. */
    Result<void> VerifyArtifactEnvelope(const std::span<const std::uint8_t> bytes, const AssetCookManifestEntry &entry,
                                        const AssetCookTargetId &target, const AssetCookLimits &limits);

    /** @brief Creates a canonical host-identified operation namespace without adopting existing staging. */
    Result<std::filesystem::path> CreateOperationRoot(const std::filesystem::path &root, const AssetCookPublicationPolicy &policy);

    /** @brief Verifies current authority or durably initializes a virgin unpublished selector before immutable promotion. */
    Result<void> EnsurePublicationBaseline(const std::filesystem::path &root, const AssetCookTargetId &target,
                                           const AssetCookLimits &limits, const AssetCookPublicationPolicy &policy);
}  // namespace Horo::Assets::CookStorageDetail
