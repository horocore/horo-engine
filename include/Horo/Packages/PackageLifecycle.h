#pragma once

/** @file PackageLifecycle.h
 * @brief Explicit verified package-to-native-host activation transaction and lifetime owner.
 */

#include "Horo/Assets/AssetImporter.h"
#include "Horo/Extensions/ExtensionCapabilityAdmission.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Packages/PackageInstall.h"
#include "Horo/Packages/PackageModuleHost.h"

#include <optional>

namespace Horo::Extensions {
    class EditorActivityHost;
}

namespace Horo::Packages {
    /** @brief Explicit portable enablement selection; it carries no execution trust or host path. */
    struct EnabledPackageExtension final {
        HoroPackageId package;
        PackagePath descriptor;
    };

    /** @brief Local/policy approval bound to the exact installed package bytes, separate from enablement. */
    struct PackageExtensionTrustApproval final {
        Sha256Digest artifactDigest;
        Sha256Digest manifestDigest;
        Extensions::ExtensionAdmissionPolicy policy;
    };

    /** @brief External trust authority; portable metadata cannot implement an implicit approval. */
    class IPackageExtensionTrust {
    public:
        virtual ~IPackageExtensionTrust() = default;
        /**
         * @brief Evaluates native execution for this exact immutable install and descriptor.
         * @param package Verified installed package and publisher evidence.
         * @param descriptor Validated descriptor read from the same archive.
         * @return Exact local/policy approval or typed trust-required failure, including in noninteractive hosts.
         */
        [[nodiscard]] virtual Result<PackageExtensionTrustApproval> Approve(const PackageRestorePackage &package,
                                                                            const Extensions::ExtensionManifest &descriptor) const = 0;
    };

    /** @brief Runtime boundaries where composition is permitted; normal runtime mutation is rejected. */
    enum class PackageActivationBoundary : std::uint8_t {
        Startup,
        Quiescent,
        Running
    };
    /** @brief Last attempt disposition; live state remains separate and survives a failed attempt. */
    enum class PackageActivationOutcome : std::uint8_t {
        NotAttempted,
        Preparing,
        Succeeded,
        Failed,
        Cancelled,
        Closed
    };

    /** @brief Host-owned construction policy; no package can choose its own loader, gate or temporary root. */
    struct PackageLifecycleConfiguration final {
        std::filesystem::path temporaryRoot; /**< Canonical existing host-owned temporary parent, retained through drain. */
        Extensions::ExtensionHostProfile profile{Extensions::ExtensionHostProfile::Headless};
        std::vector<std::string> capabilities;
        std::shared_ptr<const Security::NativeArtifactGate> artifactGate;
        Extensions::ExtensionManager::NativeLibraryLoader libraryLoader;
        std::size_t maximumExtensions{64U};
        std::size_t maximumRetiredCompositions{16U}; /**< Held callbacks/catalogs cannot cause unbounded native retention. */
        std::vector<std::shared_ptr<IPackageModuleHost>> moduleHosts;
        std::shared_ptr<Extensions::EditorActivityHost> editorActivityHost; /**< Explicit presentation owner, never discovered. */
        std::shared_ptr<PackageModuleRetirementQueue> moduleRetirements;
        /**< Explicit drain owner required for mixed-role hosts. */ /**< Explicit owning hosts, selected before activation. */
    };

    /** @brief Immutable published graph, actual registrations and real activation-generation evidence. */
    struct PackageActivationSnapshot final {
        std::shared_ptr<const VerifiedPackageInstallRecord> install;
        std::uint64_t generation{};
        std::vector<Extensions::ExtensionActivationLease> activations;
        std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> importers;
        std::vector<PackageModuleReport> modules; /**< Independent role outcomes from the exact published composition. */
    };

    /** @brief Transient journal recorded before native mutation; failure does not rewrite the active snapshot. */
    struct PackageLifecycleState final {
        PackageActivationOutcome outcome{PackageActivationOutcome::NotAttempted};
        std::uint64_t attemptedGeneration{};
        std::uint64_t installRevision{};
        std::optional<Error> diagnostic;
        bool restartRequired{};
        std::vector<PackageModuleReport> modules; /**< Attempted role outcomes, including rejected unpublished candidates. */
    };

    /**
     * @brief Owns explicit all-or-nothing verified package activation on the host composition lane.
     * @details Installation remains availability-only. Create, Activate, Query, FinalizeRetirements and Shutdown
     * belong to one host thread. The application must quiesce runtime consumers and serialize installs with
     * activation at Startup/Quiescent boundaries. Snapshots may be retained, but callers must release them
     * to permit owner-lane native finalization. Retained adapters reject new work after retirement.
     */
    class PackageLifecycleService final {
    public:
        /**
         * @brief Composes the native host from explicit filesystem, trust and installation authorities.
         * @param install Borrowed availability authority, which outlives this service.
         * @param trust Borrowed local/policy authority, which outlives this service.
         * @param configuration Native loader, signature gate, host profile and bounded storage policy.
         * @return Inert service or typed configuration failure; no code loads during construction.
         */
        [[nodiscard]] static Result<std::unique_ptr<PackageLifecycleService>> Create(PackageInstallService &install,
                                                                                     const IPackageExtensionTrust &trust,
                                                                                     PackageLifecycleConfiguration configuration);
        ~PackageLifecycleService() noexcept;
        PackageLifecycleService(const PackageLifecycleService &) = delete;
        PackageLifecycleService &operator=(const PackageLifecycleService &) = delete;
        /**
         * @brief Activates one complete enabled selection against the current sealed install record.
         * @param enabled Explicit package-relative descriptor selections; dependency order is derived from the sealed lock graph.
         * @param boundary Host lifecycle safe point; Running fails before native work.
         * @param cancellation Cancellation checked throughout staging and immediately before publication.
         * @return Success only after actual native activation and registration; failures retain the last usable graph.
         * @details Cancellation after the atomic publication point cannot reverse a successful commit.
         */
        [[nodiscard]] Result<void> Activate(std::span<const EnabledPackageExtension> enabled, PackageActivationBoundary boundary,
                                            const CancellationToken &cancellation);
        /** @brief Routes selected verified modules to their explicit owning hosts at a quiescent boundary.
         * @param enabled Exact package/descriptor/module/role selections and required/optional policy.
         * @param boundary Startup or quiescent host lifecycle point; Running fails before host work.
         * @param cancellation Cooperative cancellation before the atomic composition publication.
         * @return Success with independent optional outcomes, or required-role failure retaining the prior composition. */
        [[nodiscard]] Result<void> ActivateModules(std::span<const EnabledPackageModule> enabled, PackageActivationBoundary boundary,
                                                   const CancellationToken &cancellation);
        /** @brief Checks exact injected availability authority without reading mutable package state.
         * @param install Expected stable service instance. @return Whether this lifecycle borrows that exact authority. */
        [[nodiscard]] bool UsesInstallService(const PackageInstallService &install) const noexcept;

        /** @brief Returns the last fully published composition. @return Owning immutable snapshot or empty. */
        [[nodiscard]] std::shared_ptr<const PackageActivationSnapshot> Active() const;
        /** @brief Returns the attempt journal and drain state. @return Owner-lane inspection copy. */
        [[nodiscard]] PackageLifecycleState State() const;
        /** @brief Finalizes reverse-order retirement after outstanding importer/callback leases are released. */
        void FinalizeRetirements();
        /** @brief Revokes admissions and retires native owners; repeat calls are safe. Busy owners require restart. */
        void Shutdown() noexcept;

    private:
        struct Impl;
        explicit PackageLifecycleService(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Packages
