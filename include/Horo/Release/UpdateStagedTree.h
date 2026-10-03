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
#include <vector>

namespace Horo::Release {
    /** @brief Reserved archive entry for the signed, version-two file inventory. */
    inline constexpr std::string_view UpdateFileInventoryPath = "horo-update-files-v2.txt";
    /** @brief Version marker at the start of the canonical file inventory. */
    inline constexpr std::string_view UpdateFileInventoryHeader = "horo-update-files-v2\n";

    /** @brief Portable executable intent serialized as exact POSIX permission bits. */
    enum class UpdateFileMode : std::uint8_t {
        Regular,
        Executable
    };

    /** @brief The one product entrypoint is distinguished from supporting files. */
    enum class UpdateFileRole : std::uint8_t {
        Content,
        Entrypoint
    };

    /** @brief One file declared by the authenticated package's internal inventory. */
    struct UpdateStagedFile final {
        std::string path;
        std::uint64_t size{};
        Sha256Digest digest;
        UpdateFileMode mode{UpdateFileMode::Regular};
        UpdateFileRole role{UpdateFileRole::Content};
    };

    /**
     * @brief Encodes the exact internal file inventory required in signed ZIP update packages.
     * @param files Complete package file list, excluding the reserved inventory entry, with one executable entrypoint.
     * @param limits Host policy bounds for the complete archive including its inventory entry.
     * @return Canonical sorted inventory bytes to place at UpdateFileInventoryPath before package signing.
     */
    [[nodiscard]] Result<std::string> BuildCanonicalUpdateFileInventory(std::span<const UpdateStagedFile> files,
                                                                        const UpdateArchiveLimits &limits);

    /**
     * @brief Parses a signed package's canonical inventory with the same bounds as the encoder.
     * @param bytes Complete internal inventory file, including its version header.
     * @param limits Host archive limits used to admit the surrounding package.
     * @return Exact paths, sizes, digests, executable modes, and entrypoint role or a noncanonical/unsafe inventory failure.
     */
    [[nodiscard]] Result<std::vector<UpdateStagedFile>> ParseCanonicalUpdateFileInventory(std::string_view bytes,
                                                                                          const UpdateArchiveLimits &limits);

    /**
     * @brief Verifies every staged file and rejects undeclared content, links, or nonportable paths.
     * @param root Host-owned private staging directory held quiescent during verification.
     * @param files Complete file inventory read from an already authenticated package.
     * @param limits Host policy bounds for entry count and expanded file sizes.
     * @return Success only when exact bytes and declared modes match and no additional tree entries exist.
     */
    [[nodiscard]] Result<void> VerifyUpdateStagedTree(const std::filesystem::path &root, std::span<const UpdateStagedFile> files,
                                                      const UpdateArchiveLimits &limits);
}  // namespace Horo::Release
