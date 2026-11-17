#include "Horo/Extensions/ExtensionErrors.h"
#pragma once
#include "ModuleScriptHostBridge.h"
#include "ScriptModuleReferenceInstallation.h"
#include "ScriptModuleReferenceService.h"

namespace Horo::Extensions {
    /** Real configuration/module graph owner used by statically linked builtin provider code. */
    struct ScriptModuleReferenceCode final {
        ScriptExportDescriptorSnapshotPtr schema;
        ExtensionModulePlan graph;
        std::uint64_t generation{};
    };

    struct ScriptModuleReferenceInstallation::Impl final {
        std::shared_ptr<ModuleScriptHostAuthority> host;
        std::shared_ptr<ScriptModuleReferenceCode> code;
        ScriptCapabilityProfile profile;
        ExtensionCapabilityAdmission consumer;
        ExtensionCapabilityAdmission providerAdmission;
        BackendServiceDescriptor descriptor;
        std::optional<ApplicationCapabilityProviderRegistration> capability;
        std::optional<BackendServiceRegistration> service;
        std::shared_ptr<ScriptInvocationProviderRegistration> invocation;

        Impl(std::shared_ptr<ModuleScriptHostAuthority> state, std::shared_ptr<ScriptModuleReferenceCode> owner,
             ScriptCapabilityProfile shape, ExtensionCapabilityAdmission caller, ExtensionCapabilityAdmission provider)
            : host(std::move(state)), code(std::move(owner)), profile(shape), consumer(std::move(caller)),
              providerAdmission(std::move(provider)) {}
    };

    [[nodiscard]] Result<ExtensionModulePlan> ScriptModuleReferenceGraph(ScriptCapabilityProfile, ScriptExportVersion);
}  // namespace Horo::Extensions
