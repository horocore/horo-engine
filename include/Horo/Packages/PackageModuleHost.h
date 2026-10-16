#pragma once

/** @file PackageModuleHost.h
 * @brief Verified package-module handoff to explicitly composed owning hosts.
 */

#include "Horo/Extensions/ExtensionCapabilityAdmission.h"
#include "Horo/Extensions/ExtensionManifest.h"
#include "Horo/Packages/PackageInstall.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Packages {
    namespace Detail {
        struct PackageModuleCandidateFactory;
        class PackageComposition;
    }  // namespace Detail
    class PackageLifecycleService;

    /** @brief Required work rolls back its candidate composition; optional failure is an attributed role outcome. */
    enum class PackageModuleRequirement : std::uint8_t {
        Required,
        Optional
    };

    /** @brief Explicit enablement selection; trust and compatibility are independently checked against installed bytes. */
    struct EnabledPackageModule final {
        HoroPackageId package;
        PackagePath descriptor;
        std::string module;
        Extensions::ExtensionModuleRole role{Extensions::ExtensionModuleRole::BackendCapability};
        PackageModuleRequirement requirement{PackageModuleRequirement::Required};
    };

    /** @brief One role's immutable lifecycle disposition, separate from package installation. */
    enum class PackageModuleOutcome : std::uint8_t {
        Installed,
        Enabled,
        Trusted,
        Compatible,
        Loaded,
        Active,
        Unavailable,
        Denied,
        Incompatible,
        Failed,
        Retiring,
        Retired
    };

    /** @brief Bounded attributed evidence; the original error remains actionable across the host boundary. */
    struct PackageModuleReport final {
        HoroPackageId package;
        std::string module;
        Extensions::ExtensionModuleRole role;
        std::string host;
        std::uint64_t generation{};
        PackageModuleOutcome outcome{PackageModuleOutcome::Installed};
        std::optional<Error> diagnostic;
        std::vector<PackageModuleOutcome> phases; /**< Actual completed admission/host milestones in order. */
    };

    /** @brief Read-only, unforgeable exact install/descriptor/module/grant handoff; owns no native handle or raw host path. */
    class VerifiedPackageModule final {
    public:
        /** @brief Returns the sealed installed graph owning all declared resource bytes. @return Exact retained availability record. */
        [[nodiscard]] const std::shared_ptr<const VerifiedPackageInstallRecord> &Install() const noexcept;
        /** @brief Returns the immutable selected package. @return Exact archive and lock evidence. */
        [[nodiscard]] const PackageRestorePackage &Package() const noexcept;
        /** @brief Returns the validated descriptor from the same archive. @return Immutable complete descriptor. */
        [[nodiscard]] const Extensions::ExtensionManifest &Manifest() const noexcept;
        /** @brief Returns the exact declared module. @return Immutable module; never a caller-created replacement. */
        [[nodiscard]] const Extensions::ExtensionModuleManifest &Module() const noexcept;
        /** @brief Returns exact role/generation/host attribution. @return Immutable admitted report. */
        [[nodiscard]] const PackageModuleReport &Identity() const noexcept;
        /** @brief Returns the validated package-relative descriptor identity. @return No host filesystem path. */
        [[nodiscard]] const PackagePath &Descriptor() const noexcept;
        /** @brief Returns the exact compatibility-selected declared artifact. @return Package-relative verified identity. */
        [[nodiscard]] const PackagePath &Artifact() const noexcept;
        /** @brief Returns exact local dependency and required service-provider module IDs. @return Validated bounded dependency set. */
        [[nodiscard]] std::span<const std::string> RequiredModules() const noexcept;
        /** @brief Returns the revocable module capability grant. @return Proof invalidated before teardown. */
        [[nodiscard]] const Extensions::ExtensionActivationLease &Activation() const noexcept;
        /** @brief Borrows exact declared and approved capability handles for host binding composition. @return Revocable module-owned
         * grants. */
        [[nodiscard]] std::span<const Extensions::ExtensionCapabilityHandle> Capabilities() const noexcept;
        /** @brief Reads a declared file only within this descriptor's verified contribution root.
         * @param path Exact package-relative inventory identity. @param maximumBytes Allocation bound.
         * @return Owned bytes or the original typed containment/size/read error. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadDeclaredFile(const PackagePath &path, std::uint64_t maximumBytes) const;

    private:
        friend struct Detail::PackageModuleCandidateFactory;
        struct State;
        /** @brief Retains only a lifecycle-validated immutable handoff. @param state Sealed module/grant evidence. */
        explicit VerifiedPackageModule(std::shared_ptr<const State> state) noexcept;
        std::shared_ptr<const State> state_;
    };

    /** @brief Explicit owner-lane drain authority that outlives lifecycle services and their outstanding callbacks.
     * The application must retain this owner until Drain returns zero; abandoning busy executable work terminates.
     */
    class PackageModuleRetirementQueue final {
    public:
        /** @brief Creates a bounded retirement owner. @param maximumCompositions Positive retained composition limit. */
        explicit PackageModuleRetirementQueue(std::size_t maximumCompositions = 16);
        ~PackageModuleRetirementQueue() noexcept;
        PackageModuleRetirementQueue(const PackageModuleRetirementQueue &) = delete;
        PackageModuleRetirementQueue &operator=(const PackageModuleRetirementQueue &) = delete;
        /** @brief Finalizes drained generations only on the creating thread. @return Number still retained. */
        [[nodiscard]] std::size_t Drain();
        /** @brief Attempts bounded shutdown on the owner lane. @return Typed failure while any work remains; retain this drain handle. */
        [[nodiscard]] Result<void> Close();
        /** @brief Returns occupied retained slots. @return Pending retired compositions. */
        [[nodiscard]] std::size_t Pending() const noexcept;

    private:
        friend class PackageLifecycleService;
        struct State;
        std::shared_ptr<State> state_;
        /** @brief Reserves a lifecycle slot before any callbacks. @return False on wrong lane or full queue. */
        [[nodiscard]] bool Reserve() noexcept;
        /** @brief Transfers a reserved generation, preserving the entire verified resource closure. */
        void Retain(std::unique_ptr<Detail::PackageComposition> composition) noexcept;
        /** @brief Releases one unused reservation. */
        void Release() noexcept;
    };

    /** @brief Actual owning-host registration and resource lifetime; destruction must not unload undrained executable work. */
    class IPackageModuleActivation {
    public:
        virtual ~IPackageModuleActivation() = default;
        /** @brief Reports actual owning-host registration liveness. @return True only after live publication. */
        [[nodiscard]] virtual bool IsActive() const noexcept = 0;

        /** @brief Preserves an activation failure while its rollback receipt still owns draining work. @return Original owning-host error.
         */
        [[nodiscard]] virtual std::optional<Error> Diagnostic() const {
            return {};
        }

        /** @brief Revokes new work and withdraws registrations; repeated calls are safe. */
        virtual void Retire() noexcept = 0;
        /** @brief Finalizes on the owning composition lane. @return False while callbacks/resources still retain this generation. */
        [[nodiscard]] virtual bool Finalize() noexcept = 0;
    };

    /** @brief Owning subsystem adapter injected by application composition; no backend discovery or selection occurs here. */
    class IPackageModuleHost {
    public:
        virtual ~IPackageModuleHost() = default;
        /** @brief Returns bounded stable owning-host attribution. @return Nonempty canonical host identity. */
        [[nodiscard]] virtual std::string_view Identity() const noexcept = 0;
        /** @brief Reports explicit role admission. @param role Declared module role. @return Whether this host owns that role. */
        [[nodiscard]] virtual bool Supports(Extensions::ExtensionModuleRole role) const noexcept = 0;
        /** @brief Activates verified work only at a host-owned quiescent boundary.
         * @param module Exact validated install/module/grant handoff, retained by the returned owner.
         * @param dependencies Live required module owners, retained through teardown in reverse order.
         * @param cancellation Cancellation checked before registration and publication.
         * @return Actual owning-host registration/resource owner, or original typed failure; failed work rolls back internally. */
        [[nodiscard]] virtual Result<std::unique_ptr<IPackageModuleActivation>> Activate(
            const VerifiedPackageModule &module, std::span<const std::shared_ptr<IPackageModuleActivation>> dependencies,
            const CancellationToken &cancellation) = 0;
    };
}  // namespace Horo::Packages
