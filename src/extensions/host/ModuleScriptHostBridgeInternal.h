#pragma once
#include "ModuleScriptHostBridge.h"
#include "NativeExtensionServiceInternal.h"

namespace Horo::Extensions {
    struct ModuleScriptHostAuthority::Impl final {
        std::shared_ptr<NativeExtensionServiceHost> host;
        std::shared_ptr<NativeExtensionServiceState> state;

        explicit Impl(std::shared_ptr<NativeExtensionServiceState> actual) : state(std::move(actual)) {}

        explicit Impl(std::shared_ptr<NativeExtensionServiceHost> owner)
            : host(std::move(owner)), state(NativeExtensionServiceHostAccess::State(*host)) {}
    };

    struct ModuleScriptNativePort::Impl final {
        std::shared_ptr<NativeExtensionServiceState> state;
        ModuleScriptNativeEvidence evidence;

        Impl(std::shared_ptr<NativeExtensionServiceState> owner, ModuleScriptNativeEvidence actual)
            : state(std::move(owner)), evidence(std::move(actual)) {}

        Result<BackendServiceCall<NativeScalarService>> Resolve() const;
    };
}  // namespace Horo::Extensions
