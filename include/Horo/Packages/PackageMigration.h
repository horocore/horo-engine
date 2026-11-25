#pragma once

/** @file PackageMigration.h @brief Verified declarations for package asset path migration previews. */

#include "Horo/Packages/PackageArchive.h"
#include "Horo/Packages/PackageLockfile.h"

namespace Horo::Packages {
    inline constexpr std::string_view PackageAssetRenameDeclarationPath = "migrations/asset-renames.json";

    /** @brief One verified archive path transition inside the same portable package. */
    struct PackageAssetRename final {
        PackagePath previous;
        PackagePath candidate;
        bool preservesBytes{}; /**< False when following the new path also changes payload bytes. */
    };

    /** @brief Reads a bounded data-only rename declaration from the exact candidate archive.
     * @param previous Installed lock entry. @param previousArchive Independently verified old archive.
     * @param candidate Proposed lock entry. @param candidateArchive Independently verified new archive.
     * @return The declared rename set, empty when absent or inapplicable, or a typed validation error.
     * @details The plan is bound to the old artifact digest. Paths must move from existing old assets to new candidate assets
     * without colliding with other declarations or referencing executable files. No project writes or approval occur here.
     */
    [[nodiscard]] Result<std::vector<PackageAssetRename>> InspectPackageAssetRenames(
        const LockedPackage &previous, const ValidatedPackageArchive &previousArchive, const LockedPackage &candidate,
        const ValidatedPackageArchive &candidateArchive);
}  // namespace Horo::Packages
