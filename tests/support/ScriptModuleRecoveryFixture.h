#pragma once
#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "ScriptModuleRuntimeFixture.h"

#include <algorithm>
#include <thread>

namespace Horo::Extensions::Tests::RecoverySupport {
    using namespace ModuleSupport;

    struct Adapter final {
        ScriptModuleRuntimeIdentity (*identity)(std::string);
        Result<std::unique_ptr<IScriptModuleRuntime>> (*create)(std::shared_ptr<ScriptModuleContext>);
        const char *pending;
        const char *simple;
    };

    inline constexpr std::array<Adapter, 2> Adapters{
        {{LuaScriptModuleRuntimeIdentity, CreateLuaScriptModuleRuntime, R"(local r=horo.import("reference").horo.reference
local held=r.sum
local localValue=41
r.digest("abc")
if horo.import("reference").horo.reference.apirevision().values[1]~="1.1.0" then error("lost candidate namespace") end
return held(localValue))",
          R"(return horo.import("reference").horo.reference.sum(41))"},
         {JavaScriptModuleRuntimeIdentity, CreateJavaScriptModuleRuntime, R"(const r=horo.import("reference").horo.reference;
const held=r.sum; const localValue=41n;
await r.digest([97,98,99]);
if(horo.import("reference").horo.reference.apirevision().values[0]!=="1.1.0")throw new Error("lost candidate namespace");
return held(localValue);)",
          R"(return horo.import("reference").horo.reference.sum(41n);)"}}};

    struct ReleaseOnExit final {
        std::shared_ptr<InvocationBlock> block;
        bool released{};

        void Release() {
            if (!released) {
                released = true;
                block->release.release();
            }
        }

        ~ReleaseOnExit() {
            Release();
        }
    };

    inline ResolvedExtensionServiceImport CandidateImport() {
        ExtensionManifest manifest;
        manifest.id = "horo.reference.package";
        auto provider = Provider();
        provider.version = "1.1.0";
        provider.exports.front().version = "1.1.0";
        manifest.modules = {Consumer(), std::move(provider)};
        return Take(ResolveExtensionModules(manifest, Environment())).serviceImports.front();
    }

    inline ScriptExportDescriptorSnapshotPtr Schema(bool breaking) {
        auto result = Take(BuildScriptModuleReferenceApi("horo.reference.provider", {1, 1, 0}));
        if (!breaking)
            return result;
        auto descriptors = std::vector<ScriptExportDescriptor>{result->Descriptors().begin(), result->Descriptors().end()};
        std::erase_if(descriptors.front().functions, [](const auto &f) {
            return f.id == "sum";
        });
        return Take(BuildScriptExportDescriptorSnapshot(descriptors));
    }

    /** Genuine second typed provider in the SAME real registries; the old registration remains admitted. */
    struct Candidate final {
        Fixture &host;
        std::shared_ptr<Audit> audit{std::make_shared<Audit>()};
        ExtensionCapabilityAdmission admission{Admission("horo.reference.provider", 8)};
        ScriptExportDescriptorSnapshotPtr schema;
        BackendServiceDescriptor descriptor;
        std::optional<ApplicationCapabilityProviderRegistration> capability;
        std::optional<BackendServiceRegistration> service;
        std::shared_ptr<ScriptInvocationProviderRegistration> invocation;

        explicit Candidate(Fixture &actual, bool breaking = false) : host(actual), schema(Schema(breaking)), descriptor(actual.descriptor) {
            descriptor.version = {1, 1, 0};
            descriptor.provider.generation = 8;
            capability.emplace(Take(host.capabilities->Register({descriptor.capability, descriptor.version, descriptor.provider})));
            service.emplace(Take(host.services->Register(descriptor, std::make_unique<ObservedService>(schema, audit),
                                                         BackendServiceCodeLease::Retain(audit))));
            invocation =
                std::make_shared<ScriptInvocationProviderRegistration>(Take(host.invocations->RegisterProvider({.generation = 8})));
        }

        ~Candidate() {
            invocation->Reset();
            if (service)
                static_cast<void>(service->Reset());
        }

        std::shared_ptr<const ScriptRuntimeBackendBinding> Binding() {
            ScriptRuntimeBackendBindingInput input{CandidateImport(), Take(host.consumer.Grant(descriptor.capability)),
                                                   Take(admission.Grant(descriptor.capability)), BackendServiceCodeLease::Retain(audit)};
            input.service = descriptor;
            input.capabilities = host.capabilities;
            input.services = host.services;
            input.provider = invocation;
            input.descriptors = schema;
            input.apiId = "horo.reference.api";
            input.versions = {{1, 0, 0}, {1, 1, 0}};
            input.profile = ScriptCapabilityProfile::Tooling;
            for (const auto &f : schema->Find(input.apiId)->functions)
                input.functions.push_back(f.id);
            return Take(BindScriptRuntimeBackend<ObservedService>(std::move(input), &ObservedService::Invoke));
        }

        std::shared_ptr<ScriptModuleContext> RawContext(const JobSystem &jobs, ScriptModuleBindingCache &cache,
                                                        ScriptModuleRuntimeIdentity runtime) {
            std::vector<ScriptModuleImport> imports;
            imports.push_back(Take(BindScriptModuleImport("reference", Binding())));
            return Take(ScriptModuleContext::Create(host.invocations, jobs, cache, std::move(runtime), host.Policy(), std::move(imports)));
        }

        std::shared_ptr<ScriptModuleContext> Context(ScriptModuleContext &previous) {
            std::vector<ScriptModuleImport> imports;
            imports.push_back(Take(BindScriptModuleImport("reference", Binding())));
            return Take(previous.PrepareReplacement(std::move(imports)));
        }
    };

    inline void Finish(IScriptModuleRuntime &runtime) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!runtime.ResultSnapshot() && std::chrono::steady_clock::now() < deadline) {
            Accept(runtime.Poll());
            std::this_thread::yield();
        }
        const auto result = runtime.ResultSnapshot();
        REQUIRE(result);
        REQUIRE(result->IsSuccess());
        REQUIRE(result->values.size() == 1);
        REQUIRE(result->values.front().AsSignedInteger());
        CHECK(*result->values.front().AsSignedInteger() == 42);
    }

    inline void Commit(IScriptModuleRuntime &runtime, std::shared_ptr<ScriptModuleContext> next) {
        auto prepared = Take(runtime.PrepareRebind(std::move(next)));
        Accept(prepared->Validate());
        REQUIRE(prepared->Commit());
    }

    inline ResolvedExtensionServiceImport OptionalResolution(bool present) {
        ExtensionManifest manifest;
        manifest.id = "horo.reference.package";
        manifest.modules = {Consumer(false)};
        if (present)
            manifest.modules.push_back(Provider());
        return Take(ResolveExtensionModules(manifest, Environment())).serviceImports.front();
    }

    inline std::vector<ScriptModuleImport> OptionalImports(Fixture &host, bool present) {
        std::vector<ScriptModuleImport> imports;
        if (present) {
            auto input = host.Input();
            input.import = OptionalResolution(true);
            auto binding = Take(BindScriptRuntimeBackend<ObservedService>(std::move(input), &ObservedService::Invoke));
            imports.push_back(Take(BindScriptModuleImport("optional", std::move(binding))));
        } else
            imports.push_back(Take(MakeUnavailableScriptModuleImport("optional", OptionalResolution(false))));
        imports.push_back(Take(BindScriptModuleImport("reference", host.Binding())));
        return imports;
    }

}  // namespace Horo::Extensions::Tests::RecoverySupport
