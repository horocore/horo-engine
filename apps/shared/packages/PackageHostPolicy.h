#pragma once
/** @file PackageHostPolicy.h @brief Conservative executable-owned package execution policy and native platform facts. */
#include "Horo/Packages/PackageLifecycleErrors.h"
#include "Horo/Security/SecurityErrors.h"
#include "PackageProjectSession.h"

namespace Horo::Application::Internal {
    /** @brief Missing host execution approval fails closed; no presentation boolean grants native execution. */
    class UnconfiguredPackageTrust final : public Packages::IPackageExtensionTrust {
    public:
        [[nodiscard]] Result<Packages::PackageExtensionTrustApproval> Approve(const Packages::PackageRestorePackage &,
                                                                              const Extensions::ExtensionManifest &) const override {
            return Result<Packages::PackageExtensionTrustApproval>::Failure(MakeError(Packages::PackageLifecycleErrors::TrustRequired));
        }
    };

    /** @brief The explicit host cannot load an image without a configured signature authority. */
    class UnconfiguredPackageGate final : public Security::NativeArtifactGate {
    public:
        [[nodiscard]] Result<Security::VerifiedArtifactEvidence> Verify(const std::filesystem::path &) const override {
            return Result<Security::VerifiedArtifactEvidence>::Failure(MakeError(SecurityErrors::MissingEvidence));
        }
    };

    /** @brief Selects native package compatibility facts from the executable build, never from package metadata.
     * @return Actual operating system/architecture and extension ABI family; final native fingerprint validation remains mandatory. */
    [[nodiscard]] Result<Packages::PackagePlatform> NativePackagePlatform();
}  // namespace Horo::Application::Internal
