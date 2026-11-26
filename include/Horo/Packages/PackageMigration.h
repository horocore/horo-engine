#pragma once

/** @file PackageMigration.h @brief Artifact-bound package migration declarations for previews. */

#include "Horo/Packages/PackageArchive.h"
#include "Horo/Packages/PackageLockfile.h"

namespace Horo::Packages {
    inline constexpr std::string_view PackageAssetRenameDeclarationPath = "migrations/asset-renames.json";
    inline constexpr std::string_view PackageBehaviorRenameDeclarationPath = "migrations/behavior-renames.json";

    /** @brief Identifies the reserved declaration file when it is inert archive metadata.
     * @param file Verified file inventory entry.
     * @return True for the reserved path with neither executable bytes nor a contribution owner.
     */
    [[nodiscard]] bool IsPackageAssetRenameMetadata(const PackageFileEntry &file);

    /** @brief Validates historical asset rename metadata when there is no installed predecessor.
     * @param candidate Proposed lock entry.
     * @param archive Independently verified candidate archive.
     * @return Success when absent or valid, otherwise a typed declaration error.
     */
    [[nodiscard]] Result<void> ValidatePackageAssetRenameDeclaration(const LockedPackage &candidate,
                                                                     const ValidatedPackageArchive &archive);

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
     * without portable path collisions in the declaration or across the inventories. Executable files are excluded.
     */
    [[nodiscard]] Result<std::vector<PackageAssetRename>> InspectPackageAssetRenames(
        const LockedPackage &previous, const ValidatedPackageArchive &previousArchive, const LockedPackage &candidate,
        const ValidatedPackageArchive &candidateArchive);

    /** @brief Inert behavior names declared by the candidate archive. Gameplay owns interpretation and verification. */
    struct PackageBehaviorRename final {
        std::string previous;
        std::string candidate;
    };

    /** @brief Reads bounded behavior rename claims scoped to one exact installed artifact.
     * @param previous Installed lock entry. @param previousArchive Independently verified old archive.
     * @param candidate Proposed lock entry. @param candidateArchive Independently verified new archive.
     * @return Declared claims, empty when absent or inapplicable, or a typed validation error.
     * @details A claim does not prove that either archive owns the named runtime type or can preserve saved fields.
     */
    [[nodiscard]] Result<std::vector<PackageBehaviorRename>> InspectPackageBehaviorRenames(
        const LockedPackage &previous, const ValidatedPackageArchive &previousArchive, const LockedPackage &candidate,
        const ValidatedPackageArchive &candidateArchive);
}  // namespace Horo::Packages
