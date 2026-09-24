#pragma once

/** @file AssetImportFileDialog.h @brief Native file picker boundary for asset import. */

#include <filesystem>
#include <optional>
#include <vector>

namespace Horo::Editor {
    /** @brief Opens the platform file picker for multiple import sources. */
    [[nodiscard]] std::vector<std::filesystem::path> ChooseAssetImportFiles();
    /** @brief Opens the platform folder picker for an import destination. */
    [[nodiscard]] std::optional<std::filesystem::path> ChooseAssetImportFolder();
}  // namespace Horo::Editor
