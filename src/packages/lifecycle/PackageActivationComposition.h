#pragma once
#include "Horo/Packages/PackageLifecycle.h"

#include <thread>

namespace Horo::Packages::Detail {
    /** @brief Immutable activation handoff, created only after archive, descriptor, artifact and trust validation. */
    struct PackageActivationCandidate final {
        std::shared_ptr<const VerifiedPackageInstallRecord> install;
        std::size_t packageIndex{};
        PackagePath descriptor;
        Extensions::ExtensionManifest manifest;
        Extensions::ExtensionModulePlan plan;
        PackageExtensionTrustApproval trust;
        std::vector<std::string> providers;
    };

    /** @brief Detached complete native host; retirement retains files until its actual work leases drain. */
    class PackageActivationComposition final {
    public:
        PackageActivationComposition(const PackageLifecycleConfiguration &configuration,
                                     std::shared_ptr<const VerifiedPackageInstallRecord> install, std::uint64_t generation);
        ~PackageActivationComposition() noexcept;
        [[nodiscard]] Result<void> Prepare(std::span<const PackageActivationCandidate> candidates,
                                           const PackageLifecycleConfiguration &configuration, const CancellationToken &cancellation);
        void Retire() noexcept;
        [[nodiscard]] bool Finalize();
        [[nodiscard]] std::shared_ptr<const PackageActivationSnapshot> Snapshot() const noexcept;

    private:
        [[nodiscard]] Result<void> Materialize(const PackageActivationCandidate &candidate, const std::filesystem::path &root,
                                               const CancellationToken &cancellation) const;
        std::filesystem::path root_;
        std::shared_ptr<PackageActivationSnapshot> snapshot_{std::make_shared<PackageActivationSnapshot>()};
        std::unique_ptr<Assets::AssetImporterCatalog> catalog_{std::make_unique<Assets::AssetImporterCatalog>()};
        std::unique_ptr<Extensions::ExtensionManager> manager_;
        std::vector<Extensions::ExtensionCapabilityAdmission> admissions_;
        std::vector<std::shared_ptr<Extensions::ExtensionRetirement>> retirements_;
        bool retired_{};
        bool removeContent_{true};
    };

    /** @brief Validates and orders every candidate before a native module is loaded. */
    [[nodiscard]] Result<std::vector<PackageActivationCandidate>> PreparePackageCandidates(
        std::shared_ptr<const VerifiedPackageInstallRecord> install, std::span<const EnabledPackageExtension> enabled,
        const IPackageExtensionTrust &trust, const PackageLifecycleConfiguration &configuration);
}  // namespace Horo::Packages::Detail
