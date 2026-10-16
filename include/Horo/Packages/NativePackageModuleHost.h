#pragma once
/** @file NativePackageModuleHost.h
 * @brief Concrete verified-package adapter to native extension and editor contribution owners.
 */
#include "Horo/Packages/PackageLifecycle.h"

namespace Horo::Packages {
    /** @brief Explicit native extension host; presentation is available only with an injected editor owner. */
    class NativePackageModuleHost final : public IPackageModuleHost {
    public:
        /** @brief Captures host configuration without activating a package.
         * @param configuration Exact security, temporary storage, profile and editor owners supplied by application composition.
         */
        explicit NativePackageModuleHost(PackageLifecycleConfiguration configuration);
        ~NativePackageModuleHost() override;
        /** @copydoc IPackageModuleHost::Identity */
        [[nodiscard]] std::string_view Identity() const noexcept override;
        /** @copydoc IPackageModuleHost::Supports */
        [[nodiscard]] bool Supports(Extensions::ExtensionModuleRole role) const noexcept override;
        /** @copydoc IPackageModuleHost::Activate */
        [[nodiscard]] Result<std::unique_ptr<IPackageModuleActivation>> Activate(
            const VerifiedPackageModule &module, std::span<const std::shared_ptr<IPackageModuleActivation>> dependencies,
            const CancellationToken &cancellation) override;
        /** @brief Captures actual importer publication for an exact live role generation.
         * @param package Owning package. @param module Owning module. @param generation Exact activation generation.
         * @return Actual host publication, or null when absent or retired.
         */
        [[nodiscard]] std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> Importers(const HoroPackageId &package,
                                                                                            std::string_view module,
                                                                                            std::uint64_t generation) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Packages
