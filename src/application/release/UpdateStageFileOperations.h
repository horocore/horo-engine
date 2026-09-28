#pragma once

#include "Horo/Foundation/Platform.h"

#include <filesystem>

namespace Horo::Release::Detail {
    /** @brief Requires a new sibling stage beneath the package's real directory. */
    [[nodiscard]] bool ValidStagePaths(const std::filesystem::path &packageFile, const std::filesystem::path &stageRoot);

    /** @brief Makes a staged tree's directory entries durable before publishing its marker. */
    [[nodiscard]] Result<void> SyncStageDirectories(const std::filesystem::path &root, NativeDurableFileSystem &files);
}  // namespace Horo::Release::Detail
