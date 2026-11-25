#pragma once

#include "Horo/Packages/PackageCache.h"
#include "Horo/Packages/PackageRequest.h"

namespace Horo::Application::Internal {
    /** @brief Gives a typed migration failure with the collected compatibility blockers. */
    [[nodiscard]] Error UpdateNeedsMigration(std::vector<Diagnostic> diagnostics = {});

    /** @brief Checks whether the old asset paths keep their bytes and contribution owners. */
    void ReviewAssetFiles(const Packages::HoroPackageId &package, const Packages::ValidatedPackageArchive &previous,
                          const Packages::ValidatedPackageArchive &candidate, std::vector<Diagnostic> &diagnostics);

    /** @brief Binds a read-only cache archive to the exact current or candidate lock entry. */
    [[nodiscard]] Result<Packages::ValidatedPackageArchive> ReadUpdateArchive(const Packages::PackageCacheStore &cache,
                                                                             const Packages::LockedPackage &locked);

    /** @brief Reports lock-level changes that need migration before publication. */
    [[nodiscard]] std::vector<Diagnostic> ReviewUpdateLock(const Packages::PackageUpdatePreview &preview);

    /** @brief Appends exact archive compatibility findings for one changed entry. */
    [[nodiscard]] Result<void> ReviewCachedChange(const Packages::PackageUpdateChange &change, const Packages::PackageCacheStore &cache,
                                                  std::vector<Diagnostic> &diagnostics);
}  // namespace Horo::Application::Internal
