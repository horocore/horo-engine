#pragma once
#include "Horo/Packages/PackageLifecycle.h"

#include <map>
#include <thread>

namespace Horo::Packages::Detail {
    /** @brief Owned verified contribution root and inert decoded descriptor. */
    struct DeclaredPackageDescriptor final {
        std::string root;
        Extensions::ExtensionManifest manifest;
    };

    /** @brief Reads and owner-binds the exact declared descriptor without host work. */
    [[nodiscard]] Result<DeclaredPackageDescriptor> ReadDeclaredPackageDescriptor(const PackageRestorePackage &package,
                                                                                  const PackagePath &descriptor);
    /** @brief Applies the owning native platform suffix to an explicit selected entry. */
    [[nodiscard]] std::string SelectedPackageNativeEntry(std::string entry);
    /** @brief Captures the native process compatibility tuple without discovering a host. */
    [[nodiscard]] Extensions::ExtensionHostEnvironment PackageHostEnvironment(const PackageLifecycleConfiguration &configuration,
                                                                              std::span<const std::string_view> capabilities);

    /** @brief Immutable activation handoff, created only after archive, descriptor, artifact and trust validation. */
    struct PackageActivationCandidate final {
        std::shared_ptr<const VerifiedPackageInstallRecord> install;
        std::size_t packageIndex{};
        PackagePath descriptor;
        Extensions::ExtensionManifest manifest;
        Extensions::ExtensionModulePlan plan;
        PackageExtensionTrustApproval trust;
        std::vector<std::string> providers;
        std::vector<std::string> selectedModules;
    };

    /** @brief One publication and retirement authority shared by native-only and mixed-role compositions. */
    class PackageComposition {
    public:
        virtual ~PackageComposition() = default;
        virtual void Retire() noexcept = 0;

        [[nodiscard]] virtual bool HasRetirementReservation() const noexcept {
            return false;
        }

        [[nodiscard]] virtual std::vector<PackageModuleReport> Reports() const {
            return {};
        }

        [[nodiscard]] virtual bool Finalize() = 0;
        [[nodiscard]] virtual std::shared_ptr<const PackageActivationSnapshot> Snapshot() const noexcept = 0;
    };

    /** @brief Detached complete native host; retirement retains files until its actual work leases drain. */
    class PackageActivationComposition final : public PackageComposition {
    public:
        PackageActivationComposition(const PackageLifecycleConfiguration &configuration,
                                     std::shared_ptr<const VerifiedPackageInstallRecord> install, std::uint64_t generation);
        ~PackageActivationComposition() noexcept override;
        [[nodiscard]] Result<void> Prepare(std::span<const PackageActivationCandidate> candidates,
                                           const PackageLifecycleConfiguration &configuration, const CancellationToken &cancellation);
        void Retire() noexcept override;
        [[nodiscard]] bool Finalize() override;
        [[nodiscard]] std::shared_ptr<const PackageActivationSnapshot> Snapshot() const noexcept override;

    private:
        /** @brief Evaluates all grants and stages verified bytes before any executable host callback. */
        [[nodiscard]] Result<void> PrepareContent(std::span<const PackageActivationCandidate> candidates,
                                                  const CancellationToken &cancellation,
                                                  std::map<std::filesystem::path, Sha256Digest> &artifacts);
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
