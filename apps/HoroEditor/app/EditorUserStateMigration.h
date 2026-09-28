#pragma once

/** @file EditorUserStateMigration.h @brief Editor host composition for legacy user-state schema migration. */

#include "Horo/Release/UserStateMigration.h"

namespace Horo::Editor {
    /**
     * @brief Upgrades legacy editor preferences and recent-project records before their stores start.
     * @param userStateRoot Host-owned user state directory, separate from projects and installation.
     * @param cacheRoot Host-owned disposable cache directory.
     * @param files Durable filesystem for backup and atomic publication.
     * @return Migration report or diagnostic; the host stops before state writers start on failure.
     */
    [[nodiscard]] Result<Release::UserStateMigrationReport> MigrateLegacyEditorUserState(const std::filesystem::path &userStateRoot,
                                                                                         const std::filesystem::path &cacheRoot,
                                                                                         NativeDurableFileSystem &files);
}  // namespace Horo::Editor
