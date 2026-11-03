#include "NativeExtensionServiceInternal.h"

#include <limits>

namespace Horo::Extensions {
    namespace {
        Result<void> Rejected(const std::string_view text) {
            return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected, std::string{text}));
        }

        bool Requested(const ExtensionModuleManifest &module, const std::string_view capability) {
            return std::ranges::find(module.requiredCapabilities, capability) != module.requiredCapabilities.end();
        }

        Result<ExtensionCapabilityAdmission> AdmitModule(const NativeExtensionServiceState &host, const NativeServiceModule &module,
                                                         const std::uint64_t generation) {
            ExtensionAdmissionRequest request{module.extension, module.declaration.id, generation, {}};
            ExtensionAdmissionPolicy policy = host.policy;
            if (std::ranges::find(policy.availableCapabilities, ExtensionCapabilityId{HORO_EXTENSION_SERVICE_CAPABILITY}) ==
                policy.availableCapabilities.end())
                return Result<ExtensionCapabilityAdmission>::Failure(Rejected("Native service feature is not available.").ErrorValue());
            std::vector<std::string> services;
            for (const auto &claim : module.declaration.exports)
                services.push_back(claim.id);
            for (const auto &claim : module.imports)
                services.push_back(claim.ServiceId());
            std::ranges::sort(services);
            services.erase(std::unique(services.begin(), services.end()), services.end());
            request.capabilities.push_back({{HORO_EXTENSION_SERVICE_CAPABILITY}, {{HORO_EXTENSION_SERVICE_PERMISSION}}});
            for (const auto &service : services) {
                if (std::ranges::find(policy.availableCapabilities, ExtensionCapabilityId{service}) == policy.availableCapabilities.end())
                    policy.availableCapabilities.push_back({service});
                request.capabilities.push_back({{service}, {{HORO_EXTENSION_SERVICE_PERMISSION}}});
            }
            return ExtensionCapabilityAdmission::Evaluate(request, policy);
        }

        Result<void> ValidateModule(const NativeServiceModule &module) {
            if (!Requested(module.declaration, HORO_EXTENSION_SERVICE_HOST_CAPABILITY))
                return Rejected("Native service transport was not declared by this module.");
            return Result<void>::Success();
        }

        Result<void> PublishService(NativeServiceModule &module, NativeServicePublication &publication) {
            const auto host = module.host.lock();
            const auto code = module.moduleCode.lock();
            const auto retirement = module.retirement.lock();
            if (!host || !code || !retirement || !publication.pending)
                return Rejected("Native service owners expired before publication.");
            const auto &shape = publication.shape;
            auto capability = host->capabilities->Register({{shape.service}, shape.version, publication.provider});
            if (capability.HasError())
                return Result<void>::Failure(capability.ErrorValue());
            publication.capability.emplace(std::move(capability).Value());
            auto service =
                host->services->Register<NativeScalarService>({{shape.service},
                                                               {shape.contract},
                                                               {shape.service},
                                                               shape.version,
                                                               publication.provider,
                                                               BackendServiceThreadRule::AnyThread},
                                                              std::move(publication.pending), BackendServiceCodeLease::Retain(code));
            if (service.HasError())
                return Result<void>::Failure(service.ErrorValue());
            publication.service.emplace(std::move(service).Value());
            if (!publication.service->AttachRetirement(retirement))
                return Rejected("Native service could not attach its actual retirement owner.");
            return Result<void>::Success();
        }

        Result<void> PublishScript(NativeServiceModule &module, NativeServicePublication &publication) {
            if (publication.shape.scriptApi.empty())
                return Result<void>::Success();
            const auto host = module.host.lock();
            auto script = host->invocations->RegisterProvider({publication.provider.generation, 64});
            if (script.HasError())
                return Result<void>::Failure(script.ErrorValue());
            publication.script = std::make_shared<ScriptInvocationProviderRegistration>(std::move(script).Value());
            return Result<void>::Success();
        }

        Result<void> AdmitStage(NativeExtensionServiceState &state) {
            if (state.owner != std::this_thread::get_id())
                return Rejected("Native service staging crossed its owner lane.");
            std::erase_if(state.modules, [](const auto &weak) {
                return weak.expired();
            });
            if (state.closed || state.modules.size() >= 256 || state.nextGeneration == 0 || !state.invocations || !state.capabilities ||
                !state.services)
                return Rejected("Native service host is closed or at capacity.");
            return Result<void>::Success();
        }
    }  // namespace

    Result<std::shared_ptr<NativeServiceModule>> NativeExtensionServiceHostAccess::Stage(
        NativeExtensionServiceHost &host, AssetImporterRegistrationSession &load,
        const std::span<const ResolvedExtensionServiceImport> imports) {
        auto &state = *host.state_;
        if (auto admitted = AdmitStage(state); admitted.HasError())
            return Result<std::shared_ptr<NativeServiceModule>>::Failure(admitted.ErrorValue());
        auto module = std::make_shared<NativeServiceModule>();
        module->host = host.state_;
        module->moduleCode = load.lifetime;
        module->retirement = load.retirement;
        module->extension = load.manifest->id;
        module->declaration = *load.extensionModule;
        for (const auto &import : imports)
            if (import.ConsumerModuleId() == module->declaration.id)
                module->imports.push_back(import);
        if (auto valid = ValidateModule(*module); valid.HasError())
            return Result<std::shared_ptr<NativeServiceModule>>::Failure(valid.ErrorValue());
        const auto generation = state.nextGeneration;
        state.nextGeneration = generation == std::numeric_limits<std::uint64_t>::max() ? 0 : generation + 1;
        auto admitted = AdmitModule(state, *module, generation);
        if (admitted.HasError())
            return Result<std::shared_ptr<NativeServiceModule>>::Failure(admitted.ErrorValue());
        module->admission.emplace(std::move(admitted).Value());
        if (!load.retirement->RegisterContribution(module->declaration.id, module))
            return Result<std::shared_ptr<NativeServiceModule>>::Failure(Rejected("Service module retirement was rejected.").ErrorValue());
        state.modules.push_back(module);
        return Result<std::shared_ptr<NativeServiceModule>>::Success(std::move(module));
    }

    Result<void> CommitNativeServices(const std::span<const std::shared_ptr<ExtensionModuleLifetime>> modules) {
        for (const auto &code : modules) {
            const auto &module = code->nativeServices;
            if (!module)
                continue;
            for (auto &publication : module->publications) {
                if (auto service = PublishService(*module, *publication); service.HasError())
                    return service;
                if (auto script = PublishScript(*module, *publication); script.HasError())
                    return script;
            }
            for (auto &client : module->clients) {
                auto provider = FindNativeService(*module->host.lock(), client->import);
                if (provider.HasError())
                    return Result<void>::Failure(provider.ErrorValue());
                client->provider = provider.Value();
            }
            module->committed.store(true, std::memory_order_release);
        }
        return Result<void>::Success();
    }

    Result<void> ValidateNativeServiceClaims(const AssetImporterRegistrationSession &load) {
        if (!load.nativeServices)
            return Result<void>::Success();
        if (load.nativeServices->publications.size() != load.extensionModule->exports.size())
            return Rejected("Module did not register every declared service export exactly once.");
        return Result<void>::Success();
    }

    NativeExtensionServiceHost::NativeExtensionServiceHost(std::shared_ptr<ApplicationCapabilityRegistry> capabilities,
                                                           std::shared_ptr<BackendServiceRegistry> services,
                                                           std::shared_ptr<ScriptInvocationRegistry> invocations,
                                                           ExtensionAdmissionPolicy policy)
        : state_(std::make_shared<NativeExtensionServiceState>(capabilities, services, std::move(invocations), std::move(policy))) {}

    NativeExtensionServiceHost::~NativeExtensionServiceHost() {
        BeginShutdown();
    }

    void NativeExtensionServiceHost::BeginShutdown() noexcept {
        state_->closed.store(true, std::memory_order_release);
        if (state_->owner != std::this_thread::get_id())
            return;  // Actual manager/retirement owners retain owner-only native finalization. Never traverse mutable tables here.
        for (auto &weak : std::ranges::reverse_view(state_->modules))
            if (const auto module = weak.lock())
                module->Revoke();
    }
}  // namespace Horo::Extensions
