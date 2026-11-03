#include "ScriptModuleReferenceInstallationInternal.h"

namespace Horo::Extensions {
    namespace {
        Result<void> Admit(const std::shared_ptr<ModuleScriptHostAuthority> &host, ScriptCapabilityProfile profile) {
            if (!host || profile >= ScriptCapabilityProfile::Count)
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
            return host->RequireOpenOwner();
        }

        Result<void> Register(auto &state) {
            auto &host = *state.host;
            const auto generation = state.code->generation;
            state.descriptor = {{"horo.script.reference"},
                                {"horo.script.reference.v1"},
                                {"horo.script.reference"},
                                {1, 0, 0},
                                {"horo.script.reference.provider", "horo.script.reference.builtin", generation},
                                BackendServiceThreadRule::AnyThread};
            auto capability =
                host.Capabilities()->Register({state.descriptor.capability, state.descriptor.version, state.descriptor.provider});
            if (capability.HasError())
                return Result<void>::Failure(capability.ErrorValue());
            state.capability.emplace(std::move(capability).Value());
            auto service = host.Services()->Register(state.descriptor, std::make_unique<ScriptModuleReferenceService>(state.code->schema),
                                                     BackendServiceCodeLease::Retain(state.code));
            if (service.HasError())
                return Result<void>::Failure(service.ErrorValue());
            state.service.emplace(std::move(service).Value());
            auto provider = host.Invocations()->RegisterProvider({.generation = generation, .maximumInvocations = 64});
            if (provider.HasError())
                return Result<void>::Failure(provider.ErrorValue());
            state.invocation = std::make_shared<ScriptInvocationProviderRegistration>(std::move(provider).Value());
            return Result<void>::Success();
        }
    }  // namespace

    ScriptModuleReferenceInstallation::ScriptModuleReferenceInstallation(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}

    ScriptModuleReferenceInstallation::~ScriptModuleReferenceInstallation() {
        Close();
    }

    Result<std::unique_ptr<ScriptModuleReferenceInstallation>> ScriptModuleReferenceInstallation::Create(
        std::shared_ptr<ModuleScriptHostAuthority> host, ScriptCapabilityProfile profile) {
        using Output = Result<std::unique_ptr<ScriptModuleReferenceInstallation>>;
        auto admitted = Admit(host, profile);
        if (admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        auto issued = host->IssueGeneration();
        if (issued.HasError())
            return Output::Failure(issued.ErrorValue());
        const auto generation = issued.Value();
        auto graph = ScriptModuleReferenceGraph(profile);
        if (graph.HasError())
            return Output::Failure(graph.ErrorValue());
        auto schema = BuildScriptModuleReferenceApi("horo.script.reference.provider");
        if (schema.HasError())
            return Output::Failure(schema.ErrorValue());
        auto caller = host->AdmitReference("horo.script.reference.consumer", generation);
        if (caller.HasError())
            return Output::Failure(caller.ErrorValue());
        auto provider = host->AdmitReference("horo.script.reference.provider", generation);
        if (provider.HasError())
            return Output::Failure(provider.ErrorValue());
        auto code = std::make_shared<ScriptModuleReferenceCode>(std::move(schema).Value(), std::move(graph).Value(), generation);
        auto state =
            std::make_unique<Impl>(std::move(host), std::move(code), profile, std::move(caller).Value(), std::move(provider).Value());
        auto owner = std::unique_ptr<ScriptModuleReferenceInstallation>{new ScriptModuleReferenceInstallation{std::move(state)}};
        auto registered = Register(*owner->impl_);
        if (registered.HasError())
            return Output::Failure(registered.ErrorValue());
        return Output::Success(std::move(owner));
    }

    Result<ScriptModuleImport> ScriptModuleReferenceInstallation::Bind(std::string alias) const {
        using Output = Result<ScriptModuleImport>;
        auto &state = *impl_;
        if (state.host->OwnerThread() != std::this_thread::get_id())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!state.invocation)
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable));
        const auto &imported = state.code->graph.serviceImports.front();
        auto caller = state.consumer.Grant(state.descriptor.capability);
        if (caller.HasError())
            return Output::Failure(caller.ErrorValue());
        auto provider = state.providerAdmission.Grant(state.descriptor.capability);
        if (provider.HasError())
            return Output::Failure(provider.ErrorValue());
        ScriptRuntimeBackendBindingInput input{imported, std::move(caller).Value(), std::move(provider).Value(),
                                               BackendServiceCodeLease::Retain(state.code)};
        input.service = state.descriptor;
        input.capabilities = state.host->Capabilities();
        input.services = state.host->Services();
        input.provider = state.invocation;
        input.descriptors = state.code->schema;
        input.apiId = "horo.reference.api";
        input.versions = {{1, 0, 0}, {1, 0, 0}};
        input.profile = state.profile;
        input.functions = {"echo", "digest", "sum", "reject"};
        input.hostClosed = state.host->ClosedFlag();
        auto port = BindScriptRuntimeBackend<ScriptModuleReferenceService>(std::move(input), &ScriptModuleReferenceService::Invoke);
        if (port.HasError())
            return Output::Failure(port.ErrorValue());
        return BindScriptModuleImport(std::move(alias), std::move(port).Value());
    }

    void ScriptModuleReferenceInstallation::Close() noexcept {
        impl_->consumer.Revoke();
        impl_->providerAdmission.Revoke();
        if (impl_->invocation)
            impl_->invocation->Reset();
        if (impl_->service)
            static_cast<void>(impl_->service->Reset());
        if (impl_->capability)
            impl_->capability->Reset();
    }
}  // namespace Horo::Extensions
