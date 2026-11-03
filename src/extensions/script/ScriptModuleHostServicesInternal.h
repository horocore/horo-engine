#include "Horo/Extensions/ExtensionErrors.h"
#pragma once
#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "ModuleScriptHostBridge.h"
#include "ScriptModuleHostServices.h"
#include "ScriptModuleNativeBridge.h"
#include "ScriptModuleReferenceInstallation.h"

namespace Horo::Extensions {
    struct ScriptModuleHostServices::Impl final {
        std::shared_ptr<ModuleScriptHostAuthority> authority;
        const JobSystem &jobs;
        ScriptCapabilityProfile profile;
        ScriptModuleBindingCache cache;
        std::unique_ptr<ScriptModuleReferenceInstallation> reference;
        const std::thread::id owner{std::this_thread::get_id()};
        std::size_t activeOperations{};
        // Explicit finite embedding profile. Capacity failure never drops or truncates an operation.
        static constexpr std::size_t MaximumOperations = 4;

        Impl(std::shared_ptr<ModuleScriptHostAuthority> actual, const JobSystem &system, ScriptCapabilityProfile shape)
            : authority(std::move(actual)), jobs(system), profile(shape) {}
    };

    struct ScriptModuleOperation::Impl final {
        std::shared_ptr<ScriptModuleHostServices> owner;
        std::unique_ptr<IScriptModuleRuntime> runtime;
        std::optional<ScriptCallResult> result;
        ScriptCallResult emergency;
        std::thread::id lane{std::this_thread::get_id()};
        bool counted{};
        ~Impl();
        Impl(std::shared_ptr<ScriptModuleHostServices> host, std::unique_ptr<IScriptModuleRuntime> vm);
    };

    [[nodiscard]] std::vector<ExtensionCapabilityId> ScriptModuleRequestedCapabilities(const ScriptModuleProgram &);
    [[nodiscard]] ScriptCapabilityContextDescriptor ScriptModuleOperationContext(const ScriptModuleProgram &, ScriptModuleOperationPolicy,
                                                                                 ScriptCapabilityProfile, std::uint64_t revision);
}  // namespace Horo::Extensions
