#pragma once

/**
 * @file UpdateStagedTree.h
 * @brief Exact file verification for a private, quiescent update staging tree.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Release/UpdateArchiveIndex.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace Horo::Release {
    /** @brief Reserved ZIP entry for the signed package's canonical file inventory. */
    inline constexpr std::string_view UpdateFileInventoryPath = "horo-update-files-v1.txt";
    /** @brief Version marker at the start of the canonical file inventory. */
    inline constexpr std::string_view UpdateFileInventoryHeader = "horo-update-files-v1\n";

    /** @brief One file declared by the authenticated package's internal inventory. */
    struct UpdateStagedFile final {
        std::string path;
        std::uint64_t size{};
        Sha256Digest digest;
    };

    /**
     * @brief Encodes the exact internal file inventory required in signed ZIP update packages.
     * @param files Complete unsigned package file list, excluding the reserved inventory entry.
     * @param limits Host policy bounds for the complete archive including its inventory entry.
     * @return Canonical sorted inventory bytes to place at UpdateFileInventoryPath before package signing.
     */
    [[nodiscard]] Result<std::string> BuildCanonicalUpdateFileInventory(std::span<const UpdateStagedFile> files,
                                                                        const UpdateArchiveLimits &limits);

    /**
     * @brief Verifies every staged file and rejects undeclared content, links, or nonportable paths.
     * @param root Host-owned private staging directory held quiescent during verification.
     * @param files Complete file inventory read from an already authenticated package.
     * @param limits Host policy bounds for entry count and expanded file sizes.
     * @return Success only when all exact bytes match and no additional tree entries exist.
     */
    [[nodiscard]] Result<void> VerifyUpdateStagedTree(const std::filesystem::path &root, std::span<const UpdateStagedFile> files,
                                                      const UpdateArchiveLimits &limits);
}  // namespace Horo::Release
