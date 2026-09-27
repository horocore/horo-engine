#pragma once

/**
 * @file UpdateArchiveIndex.h
 * @brief Bounded preflight for entries in an authenticated update archive.
 */

#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <span>
#include <string>

namespace Horo::Release {
    /** @brief Archive entry type reported by a format-specific reader before extraction. */
    enum class UpdateArchiveEntryKind : std::uint8_t {
        File,
        Directory,
        SymbolicLink,
        Other
    };

    /** @brief Exact release-portable path and expanded size read from authenticated archive metadata. */
    struct UpdateArchiveEntry final {
        std::string path;
        UpdateArchiveEntryKind kind{UpdateArchiveEntryKind::Other};
        std::uint64_t expandedBytes{};
    };

    /** @brief Host policy limits applied before any archive entry is extracted. */
    struct UpdateArchiveLimits final {
        std::uint64_t maximumEntries{};
        std::uint64_t maximumFileBytes{};
        std::uint64_t maximumExpandedBytes{};
        std::uint64_t reserveBytes{}; /**< Free capacity retained after staging. */
    };

    /**
     * @brief Rejects unsafe names, links, duplicate or colliding paths, and resource excess before extraction.
     * @param entries Complete index from a format-specific reader of an already authenticated package.
     * @param limits Nonzero host policy limits for entry count, one file, and total expanded bytes.
     * @return Success only for a bounded, portable index of regular files and directories.
     */
    [[nodiscard]] Result<void> ValidateUpdateArchiveIndex(std::span<const UpdateArchiveEntry> entries, const UpdateArchiveLimits &limits);
}  // namespace Horo::Release
