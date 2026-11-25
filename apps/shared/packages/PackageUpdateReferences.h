#pragma once

#include "Horo/Packages/PackageCache.h"
#include "Horo/Packages/PackageRequest.h"

namespace Horo::Application::Internal {
    struct PackageBehaviorMigration;
    inline constexpr std::size_t MaximumUpdateDiagnostics = 64;

    /** @brief Gives a typed migration failure with the collected compatibility blockers. */
    [[nodiscard]] Error UpdateNeedsMigration(std::vector<Diagnostic> diagnostics = {});

    /** @brief Reports files in a new locked archive that require a live package owner. */
    void ReviewAddedArchive(const Packages::LockedPackage &candidate, const Packages::ValidatedPackageArchive &archive,
                            std::vector<Diagnostic> &diagnostics);

    /** @brief Reviews sealed archives for reference changes and applicable migration declarations. */
    [[nodiscard]] Result<void> ReviewAssetFiles(const Packages::LockedPackage &previous, const Packages::ValidatedPackageArchive &previousArchive,
                                                const Packages::LockedPackage &candidate, const Packages::ValidatedPackageArchive &candidateArchive,
                                                std::vector<Diagnostic> &diagnostics);

    /** @brief Binds a read-only cache archive to the exact current or candidate lock entry. */
    [[nodiscard]] Result<Packages::ValidatedPackageArchive> ReadUpdateArchive(const Packages::PackageCacheStore &cache,
                                                                             const Packages::LockedPackage &locked);

    /** @brief Reports lock-level changes that need migration before publication. */
    [[nodiscard]] std::vector<Diagnostic> ReviewUpdateLock(const Packages::PackageUpdatePreview &preview);

    /** @brief Appends exact archive compatibility findings for one changed entry. */
    [[nodiscard]] Result<void> ReviewCachedChange(const Packages::PackageUpdateChange &change, const Packages::PackageCacheStore &cache,
                                                  std::vector<Diagnostic> &diagnostics, std::vector<PackageBehaviorMigration> &claims);
}  // namespace Horo::Application::Internal
