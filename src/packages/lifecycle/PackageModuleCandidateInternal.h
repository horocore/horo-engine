#pragma once

#include "PackageActivationComposition.h"

namespace Horo::Packages {
    struct VerifiedPackageModule::State final {
        std::shared_ptr<const VerifiedPackageInstallRecord> install;
        std::size_t packageIndex;
        PackagePath descriptor;
        PackagePath artifact;
        std::shared_ptr<const Extensions::ExtensionManifest> manifest;
        std::size_t moduleIndex;
        std::vector<std::string> requiredModules;
        PackageExtensionTrustApproval trust;
        Extensions::ExtensionModulePlan plan;
        PackageModuleReport identity;
        Extensions::ExtensionActivationLease activation;
        std::vector<Extensions::ExtensionCapabilityHandle> capabilities;
    };

    namespace Detail {
        /** @brief Only lifecycle verification can issue a module handoff and its revocable capability admission. */
        struct PackageModuleCandidateFactory final {
            [[nodiscard]] static PackageActivationCandidate NativeCandidate(const VerifiedPackageModule &module);
            [[nodiscard]] static Result<VerifiedPackageModule> Prepare(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                                       const EnabledPackageModule &request,
                                                                       const PackageLifecycleConfiguration &configuration,
                                                                       const IPackageExtensionTrust &trust, std::uint64_t generation,
                                                                       std::string_view host,
                                                                       std::optional<Extensions::ExtensionCapabilityAdmission> &admission);
        };
    }  // namespace Detail
}  // namespace Horo::Packages
