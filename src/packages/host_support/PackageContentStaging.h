#pragma once
/** @file PackageContentStaging.h @brief Non-installed shared package-host staging mechanics; no activation or backend selection. */
#include "Horo/Packages/PackageInstall.h"

namespace Horo::Packages::Detail {
    /** @brief Owning boundary error identities preserved by the shared staging implementation. */
    struct PackageStagingErrors final {
        const ErrorCodeDescriptor &invalid;
        const ErrorCodeDescriptor &storage;
        const ErrorCodeDescriptor &cancelled;
    };

    /** @brief Exclusively creates an owner-only temporary directory without adopting existing names or symlinks. */
    [[nodiscard]] Result<std::filesystem::path> CreatePrivatePackageRoot(const std::filesystem::path &parent,
                                                                         const ErrorCodeDescriptor &storageError);
    /** @brief Materializes only the exact verified descriptor contribution root into a private host-owned directory.
     * The caller retains the install and directory until its owning executable resources drain.
     */
    [[nodiscard]] Result<void> MaterializeVerifiedPackageRoot(const VerifiedPackageInstallRecord &install, std::size_t packageIndex,
                                                              const PackagePath &descriptor, const std::filesystem::path &destination,
                                                              const CancellationToken &cancellation, const PackageStagingErrors &errors);
}  // namespace Horo::Packages::Detail
