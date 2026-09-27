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

namespace Horo::Release {
    /** @brief One file declared by the authenticated package's internal inventory. */
    struct UpdateStagedFile final {
        std::string path;
        std::uint64_t size{};
        Sha256Digest digest;
    };

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
