#pragma once
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ScriptModuleRuntime.h"
#include "ScriptModuleReferenceService.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <semaphore>

namespace Horo::Extensions::Tests::ModuleSupport {
    template <class T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "ok"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline void Accept(Result<void> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "ok"));
        REQUIRE(result.HasValue());
    }

    inline ExtensionCapabilityAdmission Admission(std::string module, std::uint64_t generation) {
        return Take(ExtensionCapabilityAdmission::Evaluate({"horo.reference.package",
                                                            std::move(module),
                                                            generation,
                                                            {{{"horo.script.reference"}, {}}}},
                                                           {.revision = 1, .availableCapabilities = {{"horo.script.reference"}}}));
    }

    inline ExtensionModuleManifest Consumer(bool required = true) {
        return {.id = "horo.reference.consumer",
                .version = "1.0.0",
                .kind = "native",
                .roles = {ExtensionModuleRole::BackendCapability},
                .imports = {{.id = "reference",
                             .service = "horo.script.reference",
                             .contract = "horo.script.reference.v1",
                             .minimumVersion = "1.0.0",
                             .required = required}}};
    }

    inline ExtensionModuleManifest Provider() {
        return {.id = "horo.reference.provider",
                .version = "1.0.0",
                .kind = "native",
                .roles = {ExtensionModuleRole::BackendCapability},
                .exports = {{.id = "horo.script.reference", .contract = "horo.script.reference.v1", .version = "1.0.0"}}};
    }

    inline ExtensionHostEnvironment Environment() {
        return {.profile = ExtensionHostProfile::Headless,
                .platform = ExtensionHostPlatform::Linux,
                .architecture = ExtensionHostArchitecture::X86_64,
                .buildProfile = ExtensionBuildProfile::Debug,
                .engineVersion = "0.1.0",
                .abiMajor = 1};
    }

    inline ResolvedExtensionServiceImport Import() {
        ExtensionManifest manifest;
        manifest.id = "horo.reference.package";
        manifest.modules = {Consumer(), Provider()};
        return Take(ResolveExtensionModules(manifest, Environment())).serviceImports.front();
    }

    struct InvocationBlock final {
        std::binary_semaphore entered{0}, release{0};

        void Wait() {
            entered.release();
            static_cast<void>(release.try_acquire_for(std::chrono::seconds{3}));
        }
    };

    struct Audit final {
        std::atomic<std::size_t> calls{}, shutdown{};
        std::shared_ptr<InvocationBlock> block;
        std::function<void()> beforeInvoke;              // Real test backend callback, never an adapter execution port.
        std::optional<ScriptCallResult> providerResult;  // Immutable after invocation admission in result-validation tests.
    };

    class ObservedService final {
    public:
        ObservedService(ScriptExportDescriptorSnapshotPtr api, std::shared_ptr<Audit> audit)
            : service_(std::move(api)), audit_(std::move(audit)) {}

        Result<ScriptCallResult> Invoke(const ScriptInvocationRequest &request, const BackendServiceCallContext &context) {
            ++audit_->calls;
            if (audit_->beforeInvoke)
                audit_->beforeInvoke();
            if (audit_->block)
                audit_->block->Wait();
            if (audit_->providerResult)
                return Result<ScriptCallResult>::Success(*audit_->providerResult);
            return service_.Invoke(request, context);
        }

        void Shutdown() noexcept {
            ++audit_->shutdown;
            service_.Shutdown();
        }

    private:
        ScriptModuleReferenceService service_;
        std::shared_ptr<Audit> audit_;
    };

    struct Fixture final {
        std::shared_ptr<ApplicationCapabilityRegistry> capabilities{std::make_shared<ApplicationCapabilityRegistry>()};
        std::shared_ptr<BackendServiceRegistry> services{std::make_shared<BackendServiceRegistry>()};
        std::shared_ptr<ScriptInvocationRegistry> invocations{std::make_shared<ScriptInvocationRegistry>()};
        std::shared_ptr<Audit> audit{std::make_shared<Audit>()};
        ExtensionCapabilityAdmission consumer{Admission("horo.reference.consumer", 3)},
            providerAdmission{Admission("horo.reference.provider", 7)};
        ScriptCapabilityScope project{ScriptCapabilityScopeKind::Project}, operation{ScriptCapabilityScopeKind::Operation};
        ScriptExportDescriptorSnapshotPtr schema{Take(BuildScriptModuleReferenceApi("horo.reference.provider"))};
        BackendServiceDescriptor descriptor;
        std::optional<ApplicationCapabilityProviderRegistration> capability;
        std::optional<BackendServiceRegistration> service;
        std::shared_ptr<ScriptInvocationProviderRegistration> provider;

        explicit Fixture(BackendServiceThreadRule rule = BackendServiceThreadRule::AnyThread) {
            descriptor = {{"horo.script.reference"},
                          {"horo.script.reference.v1"},
                          {"horo.script.reference"},
                          {1, 0, 0},
                          {"horo.reference.provider", "horo.reference.provider", 7},
                          rule};
            capability.emplace(Take(capabilities->Register({descriptor.capability, descriptor.version, descriptor.provider})));
            service.emplace(Take(
                services->Register(descriptor, std::make_unique<ObservedService>(schema, audit), BackendServiceCodeLease::Retain(audit))));
            provider = std::make_shared<ScriptInvocationProviderRegistration>(Take(invocations->RegisterProvider({.generation = 7})));
        }

        ~Fixture() {
            provider->Reset();
            if (service)
                static_cast<void>(service->Reset());
        }

        ScriptRuntimeBackendBindingInput Input(std::vector<std::string> functions = {"echo", "digest", "sum", "reject"}) {
            ScriptRuntimeBackendBindingInput input{Import(), Take(consumer.Grant(descriptor.capability)),
                                                   Take(providerAdmission.Grant(descriptor.capability)),
                                                   BackendServiceCodeLease::Retain(audit)};
            input.service = descriptor;
            input.capabilities = capabilities;
            input.services = services;
            input.provider = provider;
            input.descriptors = schema;
            input.apiId = "horo.reference.api";
            input.versions = {{1, 0, 0}, schema->Find("horo.reference.api")->version};
            input.profile = ScriptCapabilityProfile::Tooling;
            input.functions = std::move(functions);
            return input;
        }

        void ReplaceProvider() {
            provider->Reset();
            service.reset();
            capability.reset();
            providerAdmission.Revoke();
            providerAdmission = Admission("horo.reference.provider", 8);
            descriptor.provider.generation = 8;
            schema = Take(BuildScriptModuleReferenceApi("horo.reference.provider", {1, 1, 0}));
            capability.emplace(Take(capabilities->Register({descriptor.capability, descriptor.version, descriptor.provider})));
            service.emplace(Take(
                services->Register(descriptor, std::make_unique<ObservedService>(schema, audit), BackendServiceCodeLease::Retain(audit))));
            provider = std::make_shared<ScriptInvocationProviderRegistration>(Take(invocations->RegisterProvider({.generation = 8})));
        }

        std::shared_ptr<const ScriptRuntimeBackendBinding> Binding() {
            return Take(BindScriptRuntimeBackend<ObservedService>(Input(), &ObservedService::Invoke));
        }

        ScriptCapabilityContextDescriptor Policy() {
            return {.scriptId = "horo.reference.program",
                    .policyRevision = 1,
                    .profile = ScriptCapabilityProfile::Tooling,
                    .packageVerified = true,
                    .packageTrusted = true,
                    .scriptTrusted = true,
                    .runtimeCompatible = true,
                    .packageCapabilities = {{"horo.script.reference"}},
                    .scriptCapabilities = {{"horo.script.reference"}},
                    .projectCapabilities = {{"horo.script.reference"}},
                    .contextCapabilities = {{"horo.script.reference"}},
                    .scopes = {project.Lease(), operation.Lease()}};
        }

        std::shared_ptr<ScriptModuleContext> Context(const JobSystem &jobs, ScriptModuleBindingCache &cache,
                                                     ScriptModuleRuntimeIdentity runtime) {
            std::vector<ScriptModuleImport> imports;
            imports.push_back(Take(BindScriptModuleImport("reference", Binding())));
            return Take(ScriptModuleContext::Create(invocations, jobs, cache, std::move(runtime), Policy(), std::move(imports)));
        }
    };
}  // namespace Horo::Extensions::Tests::ModuleSupport
