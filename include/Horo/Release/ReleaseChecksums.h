#pragma once

/**
 * @file ReleaseChecksums.h
 * @brief Deterministic checksum metadata for exact final release artifacts.
 */

#include "Horo/Release/ReleaseArtifactManifest.h"

#include <span>
#include <string>

namespace Horo::Release {
    /**
     * @brief Renders canonical SHA-256 checksum lines for post-sign artifact records.
     * @param artifacts Exact file evidence captured after all byte-changing stages, before writing checksums.txt.
     * @return Sorted `hex  relative/path` lines with one final newline, or invalid-output failure.
     * @note The checksum file and manifest must be added to final metadata without hashing themselves.
     *       This pure projection does not read files; the caller owns final-byte verification.
     */
    [[nodiscard]] Result<std::string> BuildReleaseChecksums(std::span<const ReleaseArtifactRecord> artifacts);
}  // namespace Horo::Release
