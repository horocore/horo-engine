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
    /** @brief Owner-issued guard held by the live package removal transaction.
     * @details Holds the sealed install record and the exact departing packages. A failed or abandoned attempt releases native
     * admission when its last lease ends. A published attempt keeps the old lifecycle closed to new activation until its host
     * builds a session for the new lock. Shutdown signals cancellation to the transaction before its durable publication boundary.
     * The lease does not borrow the lifecycle service and may be used or released on a worker.
     */
    class PackageRemovalLease final {
    public:
        ~PackageRemovalLease() noexcept;
        PackageRemovalLease(const PackageRemovalLease &) = delete;
        PackageRemovalLease &operator=(const PackageRemovalLease &) = delete;

        /** @brief Checks that the pending claim covers this exact project and computed graph impact.
         * @param projectRoot Canonical root of the transaction.
         * @param departing Exact removal order from the validated plan.
         * @return True only for the pending matching claim.
         */
        [[nodiscard]] bool Matches(const std::filesystem::path &projectRoot, std::span<const LockedPackageReference> departing) const;
        /** @brief Returns the verified availability used when reserving removal. @return Owning immutable archive evidence. */
        [[nodiscard]] std::shared_ptr<const VerifiedPackageInstallRecord> InstalledRecord() const noexcept;
        /** @brief Exposes owner shutdown to the transaction before publication.
         * @return An owning observer of the lifecycle's cancellation signal. */
        [[nodiscard]] CancellationToken Lifetime() const noexcept;
        /** @brief Checks whether the reserved archive can be reviewed through the configured native importer owner.
         * @param package Exact member of the sealed install being withdrawn.
         * @return True only for a pending reservation, an exact departing archive and the native backend owner.
         */
        [[nodiscard]] bool AllowsNativeImporterReview(const PackageRestorePackage &package) const;
        /** @brief Revokes activation on the old session once the lock replacement commits.
         * @pre The transaction reached its durable commit point while holding the project mutation lease. */
        void MarkPublished() const noexcept;

    private:
        friend class PackageLifecycleService;
        struct State;
        /** @brief Seals the owner-created claim. @param state Exact project, plan, and installed archive evidence. */
        explicit PackageRemovalLease(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
    };

    /** @brief Runtime role declared by a verified package, independent of activation or host compatibility. */
    struct PackageRuntimeRoleDeclaration final {
        PackagePath descriptor;
        std::string moduleId;
    };

    /** @brief Enumerates the runtime roles in one sealed installed archive without loading executable code.
     * @param install Exact sealed install. @param package Package in the installed graph.
     * @return Complete runtime declarations or a typed invalid/missing descriptor failure.
     */
    [[nodiscard]] Result<std::vector<PackageRuntimeRoleDeclaration>> InspectPackageRuntimeRoles(const VerifiedPackageInstallRecord &install,
                                                                                                const HoroPackageId &package);
    /** @brief Proves that all executable contributions of an exact verified archive belong to the gameplay role owner.
     * @param package Verified installed archive and its locked metadata.
     * @return All declared gameplay roles or a typed error for another owner or incomplete evidence.
     * @details Recognizes only native runtime modules and their owned resources. It never loads an artifact.
     */
    [[nodiscard]] Result<std::vector<PackageRuntimeRoleDeclaration>> InspectGameplayRemovalRoles(const PackageRestorePackage &package);
    /** @brief Verifies gameplay and importer-only backend withdrawal under the exact owner reservation.
     * @param package Exact sealed archive and lock being withdrawn.
     * @param reservation Owner-issued withdrawal claim after active and retiring roles have drained.
     * @return Runtime roles requiring observed reference coverage, or the exact unsupported ownership diagnostic.
     */
    [[nodiscard]] Result<std::vector<PackageRuntimeRoleDeclaration>> InspectLivePackageRemovalRoles(
        const PackageRestorePackage &package, const PackageRemovalLease &reservation);

    /** @brief Verifies live removal while retaining independently verified runtime declarations for diagnostics.
     * @param package Exact sealed archive and lock being withdrawn.
     * @param reservation Owner-issued withdrawal claim after active and retiring roles have drained.
     * @param verifiedOnFailure Replaced with declarations from verified descriptors if review fails, or cleared on success.
     * @return Complete declarations or the blocking error. Partial declarations are for diagnostics only.
     */
    [[nodiscard]] Result<std::vector<PackageRuntimeRoleDeclaration>> InspectLivePackageRemovalRoles(
        const PackageRestorePackage &package, const PackageRemovalLease &reservation,
        std::vector<PackageRuntimeRoleDeclaration> &verifiedOnFailure);

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
        Extensions::ExtensionEditorHosts editorHosts; /**< Explicit activity/page publication owners, never discovered. */
        std::shared_ptr<PackageModuleRetirementQueue> moduleRetirements;
        /**< Explicit drain owner required for mixed-role hosts. */ /**< Explicit owning hosts, selected before activation. */
        Extensions::ExtensionPlatformProviderCommit platformProviderCommit;
        /**< Explicit provider publication owner; omitted means unavailable. Captured owners outlive package retirement. */
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

        /** @brief Returns the last fully published composition on the lifecycle owner lane.
         * @return Owning immutable snapshot or empty before publication, after withdrawal, or off the owner lane. */
        [[nodiscard]] std::shared_ptr<const PackageActivationSnapshot> Active() const;
        /** @brief Finds native package owners that a replacement graph would remove.
         * @param replacement Exact candidate availability graph from restore.
         * @return Sorted unique package IDs that remain active or are still draining, or an owner-lane failure.
         * @details This read does not finalize owners or inspect project asset and service references. The application serializes
         * replacement commits with activation operations on the composition lane. */
        [[nodiscard]] Result<std::vector<HoroPackageId>> MissingRetainedPackages(const PackageRestoreGraph &replacement) const;
        /** @brief Identifies departing packages still held by active or retiring native modules.
         * @param departing Exact IDs and versions from the locked removal preview.
         * @return Sorted unique package IDs or an owner-lane failure.
         * @details Native ownership is matched by package ID. A generation from a different version of the same package can
         * still be draining. This inspection has no side effects. */
        [[nodiscard]] Result<std::vector<HoroPackageId>> RetainedRemovalPackages(std::span<const LockedPackageReference> departing) const;
        /** @brief Reserves a live removal after checking that no departing package still owns executable modules.
         * @param departing Exact nonempty set of distinct package IDs and versions from the verified installation.
         * @return Worker-safe lease or a typed owner, installed-record, or retained-module failure.
         * @details Only the composition lane can reserve. This blocks new lifecycle activation while the lease lives and after
         * publication. It does not publish metadata, mutate project documents, or unload modules.
         */
        [[nodiscard]] Result<std::shared_ptr<const PackageRemovalLease>> ReserveRemoval(
            std::span<const LockedPackageReference> departing);
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
