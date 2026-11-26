#include "Horo/Packages/PackageMigration.h"

#include <type_traits>
#include <utility>

using Horo::Packages::LockedPackage;
using Horo::Packages::ValidatedPackageArchive;
static_assert(std::is_same_v<decltype(Horo::Packages::ValidatePackageAssetRenameDeclaration(
                                 std::declval<const LockedPackage &>(), std::declval<const ValidatedPackageArchive &>())),
                             Horo::Result<void>>);
static_assert(std::is_same_v<
              decltype(Horo::Packages::InspectPackageAssetRenames(std::declval<const LockedPackage &>(),
                                                                   std::declval<const ValidatedPackageArchive &>(),
                                                                   std::declval<const LockedPackage &>(),
                                                                   std::declval<const ValidatedPackageArchive &>())),
              Horo::Result<std::vector<Horo::Packages::PackageAssetRename>>>);
static_assert(std::is_same_v<
              decltype(Horo::Packages::InspectPackageBehaviorRenames(std::declval<const LockedPackage &>(),
                                                                      std::declval<const ValidatedPackageArchive &>(),
                                                                      std::declval<const LockedPackage &>(),
                                                                      std::declval<const ValidatedPackageArchive &>())),
              Horo::Result<std::vector<Horo::Packages::PackageBehaviorRename>>>);
