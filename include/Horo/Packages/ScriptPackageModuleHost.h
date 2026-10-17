#pragma once
/** @file ScriptPackageModuleHost.h
 * @brief Verified native script providers routed to the host invocation registry.
 */
#include "Horo/Extensions/ScriptCapabilityContext.h"
#include "Horo/Packages/NativePackageModuleHost.h"

namespace Horo::Packages {
    /** @brief Application-owned invocation realm, isolated because provider generations are local to one module. */
    struct PackageScriptRegistryRealm final {
        HoroPackageId package;
        std::string module;
        std::shared_ptr<Extensions::ScriptInvocationRegistry> registry;
    };

    /** @brief Owns real script-provider registrations alongside their verified native resource receipts.
     * Language VM adapters remain outside package lifecycle; bindings use the existing typed invocation ABI.
     */
    class ScriptPackageModuleHost final : public IPackageModuleHost {
    public:
        /** @brief Captures explicitly composed invocation and native owners. @param realms Exact package/module registry authorities.
         * @param native Verified native resource/extension owner. Both must outlive role retirement.
         */
        ScriptPackageModuleHost(std::vector<PackageScriptRegistryRealm> realms, std::shared_ptr<NativePackageModuleHost> native);
        ~ScriptPackageModuleHost() override;
        /** @copydoc IPackageModuleHost::Identity */
        [[nodiscard]] std::string_view Identity() const noexcept override;
        /** @copydoc IPackageModuleHost::Supports */
        [[nodiscard]] bool Supports(Extensions::ExtensionModuleRole role) const noexcept override;
        /** @copydoc IPackageModuleHost::Activate */
        [[nodiscard]] Result<std::unique_ptr<IPackageModuleActivation>> Activate(
            const VerifiedPackageModule &module, std::span<const std::shared_ptr<IPackageModuleActivation>> dependencies,
            const CancellationToken &cancellation) override;
        /** @brief Returns exact registered-provider evidence for host capability-context construction.
         * @param package Owning package. @param module Selected module. @param generation Package activation generation.
         * @return Revocable provider lease, empty for absent/retired work.
         */
        [[nodiscard]] Extensions::ScriptInvocationProviderLease Provider(const HoroPackageId &package, std::string_view module,
                                                                         std::uint64_t generation) const;
        /** @brief Retrieves one exact module-declared capability for host context composition.
         * @param package Owning package. @param module Selected module. @param generation Activation generation.
         * @param capability Exact declared capability identity. @return Revocable approved handle, or typed denial.
         */
        [[nodiscard]] Result<Extensions::ExtensionCapabilityHandle> Capability(const HoroPackageId &package, std::string_view module,
                                                                               std::uint64_t generation,
                                                                               const Extensions::ExtensionCapabilityId &capability) const;
        /** @brief Starts a call using an already authorized context binding and exact owned provider.
         * @param package Owning package. @param module Selected module. @param generation Package activation generation.
         * @param binding Host capability-context binding. @param request Owned typed invocation request.
         * @return Invocation controller or original typed admission error.
         */
        [[nodiscard]] Result<Extensions::ScriptInvocationController> Begin(const HoroPackageId &package, std::string_view module,
                                                                           std::uint64_t generation,
                                                                           const Extensions::ScriptCapabilityBinding &binding,
                                                                           Extensions::ScriptInvocationRequest request) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Packages
