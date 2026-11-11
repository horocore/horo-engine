#include "Horo/Extensions/ScriptRuntimeBackendBinding.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>

namespace Horo::Extensions {
    namespace {
        Result<void> Invalid(std::string text) {
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid, std::move(text)));
        }

        bool Owners(const ScriptRuntimeBackendBindingInput &v) {
            return v.capabilities && v.services && v.provider && v.provider->IsRegistered() && v.descriptors &&
                   v.consumerCapability.IsUsable() && v.providerCapability.IsUsable();
        }

        bool Consumer(const ScriptRuntimeBackendBindingInput &v) {
            const auto &a = v.consumerCapability.Activation();
            return a.ExtensionId() == v.import.ConsumerExtensionId() && a.ModuleId() == v.import.ConsumerModuleId() &&
                   v.consumerCapability.Capability() == v.service.capability;
        }

        bool Provider(const ScriptRuntimeBackendBindingInput &v) {
            const auto &a = v.providerCapability.Activation();
            return a.ExtensionId() == v.import.ConsumerExtensionId() && a.ModuleId() == v.import.ProviderModuleId() &&
                   a.ModuleId() == v.service.provider.moduleId && a.Generation() == v.provider->Generation() &&
                   a.Generation() == v.service.provider.generation && v.providerCapability.Capability() == v.service.capability;
        }

        bool ImportedOwners(const ScriptRuntimeBackendBindingInput &v) {
            return v.import.Status() == ExtensionServiceImportStatus::Bound && Consumer(v) && Provider(v);
        }

        bool ServiceIdentity(const ScriptRuntimeBackendBindingInput &v) {
            return v.service.serviceId.value == v.import.ServiceId() && v.service.contractId.value == v.import.ContractId();
        }

        bool CallProfile(const ScriptRuntimeBackendBindingInput &v) {
            return v.functions.size() <= 256 && v.profile < ScriptCapabilityProfile::Count;
        }

        bool ApiVersion(const ScriptRuntimeBackendBindingInput &v, const ScriptExportDescriptor &api) {
            return v.versions.minimum.IsValid() && v.versions.maximum.IsValid() && v.versions.minimum.major == v.versions.maximum.major &&
                   v.versions.minimum <= v.versions.maximum && api.version >= v.versions.minimum && api.version <= v.versions.maximum;
        }

        bool ServiceVersion(const ScriptRuntimeBackendBindingInput &v, const ScriptExportDescriptor &api) {
            const ScriptExportVersion actual{v.service.version.major, v.service.version.minor, v.service.version.patch};
            return actual.major == api.service.minimumVersion.major && actual >= api.service.minimumVersion;
        }

        bool Api(const ScriptRuntimeBackendBindingInput &v, const ScriptExportDescriptor &api) {
            return api.moduleId == v.service.provider.moduleId && api.service.serviceId == v.import.ServiceId() &&
                   api.service.contractId == v.import.ContractId() && api.service.capability == v.service.capability &&
                   ApiVersion(v, api) && ServiceVersion(v, api);
        }

    }  // namespace

    namespace {
        Result<void> AllowedFunctions(const ScriptRuntimeBackendBindingInput &v, const ScriptExportDescriptor &api) {
            for (const auto &name : v.functions) {
                const auto method = std::ranges::find(api.functions, name, &ScriptExportFunctionDescriptor::id);
                if (method == api.functions.end() || std::ranges::count(v.functions, name) != 1)
                    return Invalid("Script function allowlist is unknown or duplicated.");
                if (method->invocation == ScriptExportInvocationMode::Asynchronous &&
                    v.service.threadRule != BackendServiceThreadRule::AnyThread)
                    return Result<void>::Failure(MakeError(ExtensionErrors::BackendServiceThreadViolation,
                                                           "Async module calls require an actual AnyThread service."));
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> Detail::ValidateScriptRuntimeBackendInput(const ScriptRuntimeBackendBindingInput &v) {
        if (!Owners(v))
            return Invalid("Script backend owners or actual grants are unavailable.");
        if (!ImportedOwners(v))
            return Invalid("Script backend import/consumer/provider generation differs.");
        const auto *api = v.descriptors->Find(v.apiId);
        if (!api || !Api(v, *api))
            return Invalid("Script descriptor does not match the actual imported service.");
        if (!ServiceIdentity(v))
            return Invalid("Concrete service identity differs from the resolved import.");
        if (!CallProfile(v))
            return Invalid("Script operation/profile admission is empty or unsupported.");
        return AllowedFunctions(v, *api);
    }

    Result<void> Detail::ValidateScriptRuntimeBackendDescriptor(const ScriptRuntimeBackendBindingInput &input,
                                                                const BackendServiceDescriptor *actual) {
        if (!actual)
            return Invalid("Concrete backend call is empty or consumed.");
        const auto &promised = input.service;
        if (const bool matches = actual->serviceId == promised.serviceId && actual->contractId == promised.contractId &&
                                 actual->capability == promised.capability && actual->version == promised.version &&
                                 actual->provider == promised.provider && actual->threadRule == promised.threadRule;
            !matches)
            return Invalid("Script execution metadata differs from the immutable actual backend registration.");
        return Result<void>::Success();
    }

    const ScriptExportDescriptor &ScriptRuntimeBackendBinding::Descriptor() const noexcept {
        return *Evidence().descriptors->Find(Evidence().apiId);
    }

    const ResolvedExtensionServiceImport &ScriptRuntimeBackendBinding::Resolution() const noexcept {
        return Evidence().import;
    }

    const ScriptRuntimeBackendBindingInput &ScriptRuntimeBackendBinding::Evidence() const noexcept {
        return port_->Input();
    }

    ScriptCapabilityImport ScriptRuntimeBackendBinding::Import() const {
        const auto &v = Evidence();
        return {v.apiId, v.versions, v.descriptors, v.providerCapability, v.provider->Lease(), v.profile, v.functions};
    }

    bool ScriptRuntimeBackendBinding::IsUsable() const noexcept {
        const auto &v = Evidence();
        return (!v.hostClosed || !v.hostClosed->load(std::memory_order_acquire)) && !v.capabilities->IsShutdown() &&
               !v.services->IsShutdown() && v.consumerCapability.IsUsable() && v.providerCapability.IsUsable() &&
               v.provider->IsRegistered();
    }

    Result<ScriptInvocationController> ScriptRuntimeBackendBinding::Begin(const ScriptCapabilityBinding &binding,
                                                                          ScriptInvocationRequest request) const {
        if (!IsUsable())
            return Result<ScriptInvocationController>::Failure(MakeError(ExtensionErrors::ScriptInvocationProviderRevoked));
        return binding.Begin(*Evidence().provider, std::move(request));
    }

    Result<ScriptCallResult> ScriptRuntimeBackendBinding::Invoke(const ScriptInvocationRequest &request,
                                                                 const CancellationToken &cancel) const {
        if (!IsUsable())
            return Result<ScriptCallResult>::Failure(MakeError(ExtensionErrors::ScriptInvocationProviderRevoked));
        return port_->Invoke(request, cancel);
    }

    Result<ScriptHandle> ScriptRuntimeBackendBinding::IssueHandle(const ScriptCapabilityBinding &binding, std::string type) const {
        if (!IsUsable())
            return Result<ScriptHandle>::Failure(MakeError(ExtensionErrors::ScriptInvocationProviderRevoked));
        return binding.IssueHandle(*Evidence().provider, std::move(type));
    }
}  // namespace Horo::Extensions

namespace Horo::Extensions {
    bool ScriptRuntimeBackendBinding::AllowsFunction(std::string_view id) const noexcept {
        return std::ranges::find(Evidence().functions, id) != Evidence().functions.end();
    }
}  // namespace Horo::Extensions
