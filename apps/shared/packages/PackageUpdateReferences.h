#pragma once

#include "Horo/Packages/PackageCache.h"
#include "Horo/Packages/PackageRequest.h"

namespace Horo::Application::Internal {
    struct PackageBehaviorMigration;
    inline constexpr std::size_t MaximumUpdateDiagnostics = 64;

    /** @brief Gives a typed migration failure with the collected compatibility blockers. */
    [[nodiscard]] Error UpdateNeedsMigration(std::vector<Diagnostic> diagnostics = {});

    /** @brief Checks the contribution policy admitted by offline package updates. */
    [[nodiscard]] bool IsAssetOnly(const Packages::LockedPackage &package);

    /** @brief Reports files in a new locked archive that require a live package owner. */
    [[nodiscard]] Result<void> ReviewAddedArchive(const Packages::LockedPackage &candidate,
                                                  const Packages::ValidatedPackageArchive &archive, std::vector<Diagnostic> &diagnostics);

    /** @brief Reviews sealed archives for reference changes and applicable migration declarations. */
    [[nodiscard]] Result<void> ReviewAssetFiles(const Packages::LockedPackage &previous, const Packages::ValidatedPackageArchive &previousArchive,
                                                const Packages::LockedPackage &candidate, const Packages::ValidatedPackageArchive &candidateArchive,
                                                std::vector<Diagnostic> &diagnostics);

    /** @brief Checks whether a retained package can be restored alongside an offline withdrawal. */
    [[nodiscard]] bool CanRestoreRetainedAsset(const Packages::LockedPackage &current, const Packages::LockedPackage &candidate);

    /** @brief Binds a read-only cache archive to the exact current or candidate lock entry. */
    [[nodiscard]] Result<Packages::ValidatedPackageArchive> ReadUpdateArchive(const Packages::PackageCacheStore &cache,
                                                                             const Packages::LockedPackage &locked);

    /** @brief Reports lock-level changes that need migration before publication. */
    [[nodiscard]] std::vector<Diagnostic> ReviewUpdateLock(const Packages::PackageUpdatePreview &preview);

    /** @brief Appends independent findings for one changed entry and retains the first review error. */
    void ReviewCachedChange(const Packages::PackageUpdateChange &change, const Packages::PackageCacheStore &cache,
                            std::vector<Diagnostic> &diagnostics, std::vector<PackageBehaviorMigration> &claims,
                            std::optional<Error> &firstFailure);
}  // namespace Horo::Application::Internal
