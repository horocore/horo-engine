#include "ModuleScriptHostBridgeInternal.h"

#include <algorithm>

namespace Horo::Extensions {
    ModuleScriptNativeEvidence::ModuleScriptNativeEvidence(ResolvedExtensionServiceImport resolved, ExtensionCapabilityHandle caller,
                                                           ExtensionCapabilityHandle providerGrant, BackendServiceCodeLease executable)
        : resolvedImport(std::move(resolved)), consumer(std::move(caller)), provider(std::move(providerGrant)),
          code(std::move(executable)) {}

    ModuleScriptNativePort::ModuleScriptNativePort(std::unique_ptr<Impl> state) : impl_(std::move(state)) {}

    ModuleScriptNativePort::~ModuleScriptNativePort() = default;

    const ModuleScriptNativeEvidence &ModuleScriptNativePort::Evidence() const noexcept {
        return impl_->evidence;
    }

    namespace {
        bool SameDescriptor(const BackendServiceDescriptor &a, const BackendServiceDescriptor &b) {
            return a.serviceId == b.serviceId && a.contractId == b.contractId && a.capability == b.capability && a.version == b.version &&
                   a.provider == b.provider && a.threadRule == b.threadRule;
        }

        Result<BackendServiceCall<NativeScalarService>> ActualCall(const NativeExtensionServiceState &state,
                                                                   const ModuleScriptNativeEvidence &v) {
            using Output = Result<BackendServiceCall<NativeScalarService>>;
            const auto &owner = v.consumer.Activation();
            auto authority =
                state.capabilities->ResolveExact(v.consumer, {v.descriptor.version, v.descriptor.version}, v.descriptor.provider,
                                                 owner.ExtensionId(), owner.ModuleId(), owner.Generation());
            if (authority.HasError())
                return Output::Failure(authority.ErrorValue());
            auto imported = state.services->BindImport(std::move(authority).Value(), v.resolvedImport);
            if (imported.HasError())
                return Output::Failure(imported.ErrorValue());
            return state.services->ResolveImported<NativeScalarService>(std::move(imported).Value());
        }

        Result<std::shared_ptr<NativeServiceModule>> Consumer(const NativeExtensionServiceState &state, std::string_view package,
                                                              std::string_view id) {
            using Output = Result<std::shared_ptr<NativeServiceModule>>;
            for (const auto &weak : state.modules) {
                auto candidate = weak.lock();
                if (!candidate)
                    continue;
                if (candidate->extension == package && candidate->declaration.id == id && candidate->committed && !candidate->revoked)
                    return Output::Success(std::move(candidate));
            }
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable));
        }

        Result<ModuleScriptNativeEvidence> Capture(const std::shared_ptr<NativeExtensionServiceState> &state,
                                                   const NativeServiceModule &consumer, const ResolvedExtensionServiceImport &resolved,
                                                   const std::shared_ptr<NativeServicePublication> &publication) {
            using Output = Result<ModuleScriptNativeEvidence>;
            const auto provider = publication->owner.lock();
            if (!provider || !consumer.admission || !provider->admission || !provider->committed || provider->revoked)
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationProviderRevoked));
            auto caller = consumer.admission->Grant({resolved.ServiceId()});
            if (caller.HasError())
                return Output::Failure(caller.ErrorValue());
            auto source = provider->admission->Grant({resolved.ServiceId()});
            if (source.HasError())
                return Output::Failure(source.ErrorValue());
            auto code = provider->moduleCode.lock();
            if (!code)
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationProviderRevoked));
            ModuleScriptNativeEvidence evidence{resolved, std::move(caller).Value(), std::move(source).Value(),
                                                BackendServiceCodeLease::Retain(std::move(code))};
            const auto &shape = publication->shape;
            evidence.descriptor = {{shape.service}, {shape.contract},      {shape.service},
                                   shape.version,   publication->provider, BackendServiceThreadRule::AnyThread};
            evidence.descriptors = publication->descriptors;
            evidence.invocation = publication->script;
            evidence.api = shape.scriptApi;
            evidence.closed = {state, &state->closed};
            for (const auto &method : shape.methods)
                evidence.functions.push_back(method.id);
            return Output::Success(std::move(evidence));
        }

        Result<void> CaptureActualDescriptor(const NativeExtensionServiceState &state, ModuleScriptNativeEvidence &evidence) {
            auto actual = ActualCall(state, evidence);
            if (actual.HasError())
                return Result<void>::Failure(actual.ErrorValue());
            const auto descriptor = actual.Value().Descriptor();
            if (!descriptor)
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable));
            evidence.descriptor = *descriptor;
            if (!evidence.invocation || !evidence.descriptors || !evidence.descriptors->Find(evidence.api))
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable));
            return Result<void>::Success();
        }
    }  // namespace

    Result<BackendServiceCall<NativeScalarService>> ModuleScriptNativePort::Impl::Resolve() const {
        using Output = Result<BackendServiceCall<NativeScalarService>>;
        if (state->closed.load(std::memory_order_acquire) || !evidence.invocation->IsRegistered())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationProviderRevoked));
        auto actual = ActualCall(*state, evidence);
        if (actual.HasError())
            return actual;
        const auto descriptor = actual.Value().Descriptor();
        if (!descriptor || !SameDescriptor(evidence.descriptor, *descriptor))
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        return actual;
    }

    Result<void> ModuleScriptNativePort::Probe() const {
        auto actual = impl_->Resolve();
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        return Result<void>::Success();
    }

    Result<ScriptCallResult> ModuleScriptNativePort::Invoke(const ScriptInvocationRequest &request, const CancellationToken &cancel) const {
        auto actual = impl_->Resolve();
        if (actual.HasError())
            return Result<ScriptCallResult>::Failure(actual.ErrorValue());
        return std::move(actual).Value().Invoke(&NativeScalarService::InvokeScript, request, cancel);
    }

    Result<ModuleScriptSourceLease> ModuleScriptHostAuthority::Source(std::string_view package, std::string_view moduleId) const {
        using Output = Result<ModuleScriptSourceLease>;
        auto lane = RequireOpenOwner();
        if (lane.HasError())
            return Output::Failure(lane.ErrorValue());
        auto selected = Consumer(*impl_->state, package, moduleId);
        if (selected.HasError())
            return Output::Failure(selected.ErrorValue());
        if (!selected.Value()->admission)
            return Output::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        auto actual = selected.Value()->admission->Grant({HORO_EXTENSION_SERVICE_CAPABILITY});
        if (actual.HasError())
            return Output::Failure(actual.ErrorValue());
        auto code = selected.Value()->moduleCode.lock();
        if (!code || !code->loaded)
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationProviderRevoked));
        return Output::Success(ModuleScriptSourceLease{std::move(actual).Value(), BackendServiceCodeLease::Retain(std::move(code))});
    }

    Result<ModuleScriptResolvedImport> ModuleScriptHostAuthority::Resolve(std::string_view package, std::string_view moduleId,
                                                                          std::string_view id) const {
        using Output = Result<ModuleScriptResolvedImport>;
        auto lane = RequireOpenOwner();
        if (lane.HasError())
            return Output::Failure(lane.ErrorValue());
        auto consumer = Consumer(*impl_->state, package, moduleId);
        if (consumer.HasError())
            return Output::Failure(consumer.ErrorValue());
        const auto &imports = consumer.Value()->imports;
        const auto found = std::ranges::find(imports, id, &ResolvedExtensionServiceImport::ImportId);
        if (found == imports.end())
            return Output::Failure(
                MakeError(ExtensionErrors::ScriptInvocationUnavailable, "The actual module does not declare this import."));
        if (found->Status() != ExtensionServiceImportStatus::Bound)
            return Output::Success(ModuleScriptResolvedImport{*found, {}});
        auto publication = FindNativeService(*impl_->state, *found);
        if (publication.HasError())
            return Output::Failure(publication.ErrorValue());
        auto evidence = Capture(impl_->state, *consumer.Value(), *found, publication.Value());
        if (evidence.HasError())
            return Output::Failure(evidence.ErrorValue());
        auto actual = std::move(evidence).Value();
        auto shape = CaptureActualDescriptor(*impl_->state, actual);
        if (shape.HasError())
            return Output::Failure(shape.ErrorValue());
        auto port = std::shared_ptr<const ModuleScriptNativePort>{
            new ModuleScriptNativePort{std::make_unique<ModuleScriptNativePort::Impl>(impl_->state, std::move(actual))}};
        return Output::Success(ModuleScriptResolvedImport{*found, std::move(port)});
    }
}  // namespace Horo::Extensions
