#pragma once
// Noninstalled lower-host bridge. No VM, higher module-runtime type, or caller callback crosses this boundary.
#include "Horo/Extensions/NativeExtensionServiceHost.h"
#include "Horo/Extensions/ScriptCapabilityContext.h"
#include "Horo/Security/ArtifactSignature.h"

#include <atomic>
#include <filesystem>

namespace Horo::Extensions {
    struct ModuleScriptNativeEvidence final {
        ResolvedExtensionServiceImport import;
        ExtensionCapabilityHandle consumer, provider;
        BackendServiceCodeLease code;
        BackendServiceDescriptor descriptor;
        ScriptExportDescriptorSnapshotPtr descriptors;
        std::shared_ptr<ScriptInvocationProviderRegistration> invocation;
        std::vector<std::string> functions;
        std::string api;
        std::shared_ptr<const std::atomic<bool>> closed;
        ModuleScriptNativeEvidence(ResolvedExtensionServiceImport, ExtensionCapabilityHandle, ExtensionCapabilityHandle,
                                   BackendServiceCodeLease);
    };
    class ModuleScriptHostAuthority;

    /** Fixed lower native member port. It owns real grants/code and re-resolves the same actual import for every call. */
    class ModuleScriptNativePort final {
    public:
        ~ModuleScriptNativePort();
        const ModuleScriptNativeEvidence &Evidence() const noexcept;
        Result<void> Probe() const;
        Result<ScriptCallResult> Invoke(const ScriptInvocationRequest &, const CancellationToken &) const;

    private:
        friend class ModuleScriptHostAuthority;
        struct Impl;
        explicit ModuleScriptNativePort(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };

    /** Existing exact selected-module admission; no new permission, callback or authority is minted. */
    class ModuleScriptSourceLease final {
    public:
        const ExtensionCapabilityHandle &Capability() const noexcept {
            return capability_;
        }

        const BackendServiceCodeLease &Code() const noexcept {
            return code_;
        }

    private:
        friend class ModuleScriptHostAuthority;

        ModuleScriptSourceLease(ExtensionCapabilityHandle grant, BackendServiceCodeLease code)
            : capability_(std::move(grant)), code_(std::move(code)) {}

        ExtensionCapabilityHandle capability_;
        BackendServiceCodeLease code_;
    };

    class ModuleScriptResolvedImport final {
    public:
        const ResolvedExtensionServiceImport &Resolution() const noexcept {
            return resolution_;
        }

        const std::shared_ptr<const ModuleScriptNativePort> &Port() const noexcept {
            return port_;
        }

    private:
        friend class ModuleScriptHostAuthority;

        ModuleScriptResolvedImport(ResolvedExtensionServiceImport r, std::shared_ptr<const ModuleScriptNativePort> p)
            : resolution_(std::move(r)), port_(std::move(p)) {}

        ResolvedExtensionServiceImport resolution_;
        std::shared_ptr<const ModuleScriptNativePort> port_;
    };

    struct ModuleScriptArtifactEvidence final {
        std::filesystem::path relative;
        Security::VerifiedArtifactEvidence evidence;
    };

    /** Composition-issued owner of the SAME canonical host/registries. Admission/table access is owner-lane only. */
    class ModuleScriptHostAuthority final {
    public:
        static Result<std::shared_ptr<ModuleScriptHostAuthority>> Create(std::shared_ptr<NativeExtensionServiceHost>);
        // Retains the actual shared state; destruction of the original host revokes this authority through its same closed flag.
        static Result<std::shared_ptr<ModuleScriptHostAuthority>> RetainState(NativeExtensionServiceHost &);
        static Result<std::shared_ptr<ModuleScriptHostAuthority>> Headless(class HeadlessExtensionHost &, bool configuring);
        ~ModuleScriptHostAuthority();
        const std::shared_ptr<ApplicationCapabilityRegistry> &Capabilities() const noexcept;
        const std::shared_ptr<BackendServiceRegistry> &Services() const noexcept;
        const std::shared_ptr<ScriptInvocationRegistry> &Invocations() const noexcept;
        const ExtensionAdmissionPolicy &Policy() const noexcept;
        std::thread::id OwnerThread() const noexcept;
        std::shared_ptr<const std::atomic<bool>> ClosedFlag() const noexcept;
        Result<void> RequireOpenOwner() const;
        Result<std::uint64_t> IssueGeneration();
        Result<ExtensionCapabilityAdmission> AdmitReference(std::string module, std::uint64_t generation) const;
        Result<ModuleScriptSourceLease> Source(std::string_view package, std::string_view module) const;
        Result<ModuleScriptResolvedImport> Resolve(std::string_view package, std::string_view module, std::string_view import) const;
        Result<void> ValidateActiveArtifacts(const ExtensionManifest &, std::span<const ModuleScriptArtifactEvidence>) const;

    private:
        struct Impl;
        explicit ModuleScriptHostAuthority(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Extensions
