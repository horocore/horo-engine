#pragma once

#include "PackageModuleCandidateInternal.h"

namespace Horo::Packages::Detail {
    /** @brief One detached mixed-role generation; every actual host/resource owner drains before its verified bytes release. */
    class PackageModuleComposition final : public PackageComposition {
    public:
        PackageModuleComposition(std::shared_ptr<const VerifiedPackageInstallRecord> install, std::uint64_t generation,
                                 bool hasRetirementReservation);
        ~PackageModuleComposition() noexcept override;
        PackageModuleComposition(const PackageModuleComposition &) = delete;
        PackageModuleComposition &operator=(const PackageModuleComposition &) = delete;
        PackageModuleComposition(PackageModuleComposition &&) = delete;
        PackageModuleComposition &operator=(PackageModuleComposition &&) = delete;
        [[nodiscard]] Result<void> Prepare(std::span<const EnabledPackageModule> enabled,
                                           const PackageLifecycleConfiguration &configuration, const IPackageExtensionTrust &trust,
                                           const CancellationToken &cancellation, const bool &shutdownRequested);
        void Retire() noexcept override;

        [[nodiscard]] bool HasRetirementReservation() const noexcept override {
            return hasRetirementReservation_;
        }

        [[nodiscard]] std::vector<PackageModuleReport> Reports() const override;
        [[nodiscard]] bool Finalize() override;
        [[nodiscard]] std::shared_ptr<const PackageActivationSnapshot> Snapshot() const noexcept override;

    private:
        struct Entry final {
            EnabledPackageModule request;
            PackageModuleReport report;
            std::shared_ptr<IPackageModuleHost> host;
            std::optional<Extensions::ExtensionCapabilityAdmission> admission;
            std::optional<VerifiedPackageModule> selectedModule;
            std::shared_ptr<IPackageModuleActivation> owner;
            std::vector<std::shared_ptr<IPackageModuleActivation>> dependencies;
            bool pending{};
        };

        [[nodiscard]] Result<void> PrepareEntries(std::span<const EnabledPackageModule> enabled,
                                                  const PackageLifecycleConfiguration &configuration, const IPackageExtensionTrust &trust);
        /** @brief Prepares one already validated role's host and admission without invoking executable factories. */
        [[nodiscard]] Result<void> PrepareHost(Entry &entry, const PackageLifecycleConfiguration &configuration,
                                               const IPackageExtensionTrust &trust) const;
        /** @brief Pins all enabled local providers, rejecting unavailable required roles before activation. */
        [[nodiscard]] Result<bool> LocalDependenciesReady(Entry &entry) const;
        [[nodiscard]] Result<bool> DependenciesReady(Entry &entry) const;
        /** @brief Advances one pending role once; false means it still waits for a provider. */
        [[nodiscard]] Result<bool> AdvanceEntry(Entry &entry, const CancellationToken &cancellation);
        [[nodiscard]] Result<void> ActivateEntry(Entry &entry, const CancellationToken &cancellation);
        [[nodiscard]] Result<void> RejectEntry(Entry &entry, Error error, PackageModuleOutcome outcome) const;
        std::shared_ptr<PackageActivationSnapshot> snapshot_{std::make_shared<PackageActivationSnapshot>()};
        std::vector<Entry> entries_;
        std::vector<std::size_t> activationOrder_;
        bool hasRetirementReservation_{};
        bool retired_{};
    };
}  // namespace Horo::Packages::Detail
