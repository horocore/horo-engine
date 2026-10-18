#pragma once

/** @file AssetRegistryPaths.h
 * @brief Target-private UTF-8 registry identity admission and native path conversion.
 */
#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Paths.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Horo::Assets::Detail {
    /** @brief Fold portable path keys using the registry's existing byte-wise case policy.
     * @param value Portable source or extension bytes.
     * @return Owned folded bytes without filesystem access.
     */
    [[nodiscard]] std::string PortableFold(std::string_view value);
    /** @brief Encode a native path for portable registry identity or diagnostic text.
     * @param path Native filesystem spelling.
     * @return Generic UTF-8 bytes, independent of the Windows ANSI code page.
     * @throws std::filesystem::filesystem_error If the native spelling cannot be encoded as UTF-8.
     */
    [[nodiscard]] std::string Utf8PathText(const std::filesystem::path &path);
    /** @brief Validate scalar UTF-8 and exclude NUL before native path conversion.
     * @param value Portable path text; root and normalization checks remain separate.
     * @return Whether the text is lossless and contains no embedded NUL.
     */
    [[nodiscard]] bool HasValidUtf8PathText(std::string_view value);
    /** @brief Admit one bounded asset-root-relative portable path.
     * @param value UTF-8 asset source or sidecar identity.
     * @return Normalized path or the existing typed invalid-root error.
     */
    [[nodiscard]] Result<ProjectPath> ParseAssetPath(std::string_view value);
    /** @brief Verify text admission and exact Prefab source/type parity.
     * @param sourcePath Portable source identity.
     * @param type Canonical asset type.
     * @return Success or the preserved invalid-root/type-mismatch error.
     */
    [[nodiscard]] Result<void> ValidateSourceType(const ProjectPath &sourcePath, const AssetTypeId &type);
}  // namespace Horo::Assets::Detail
