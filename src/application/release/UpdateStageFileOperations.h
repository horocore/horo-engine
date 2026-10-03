#pragma once

#include "Horo/Foundation/Platform.h"
#include "Horo/Release/UpdateStagedTree.h"

#include <filesystem>

namespace Horo::Release::Detail {
    /** @brief Requires a new sibling stage beneath the package's real directory. */
    [[nodiscard]] bool ValidStagePaths(const std::filesystem::path &packageFile, const std::filesystem::path &stageRoot);

    /** @brief Requires distinct private download and checkpoint siblings before an update stage is created. */
    [[nodiscard]] bool ValidStageDownloadPaths(const std::filesystem::path &packageFile, const std::filesystem::path &checkpointFile,
                                               const std::filesystem::path &stageRoot);

    /** @brief Makes a staged tree's directory entries durable before publishing its marker. */
    [[nodiscard]] Result<void> SyncStageDirectories(const std::filesystem::path &root, NativeDurableFileSystem &files);

    /** @brief Applies an authenticated mode to one new private file and makes it durable. */
    [[nodiscard]] Result<void> ApplyAuthenticatedFileMode(const std::filesystem::path &path, UpdateFileMode mode);
}  // namespace Horo::Release::Detail
