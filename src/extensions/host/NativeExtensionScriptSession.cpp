#include "NativeExtensionServiceInternal.h"

namespace Horo::Extensions {
    struct NativeExtensionScriptSession::Impl final {
        std::shared_ptr<NativeServiceModule> module;
        std::shared_ptr<NativeServicePublication> publication;
        ResolvedExtensionServiceImport import;
        ScriptCapabilityContext context;
        ScriptCapabilityBinding binding;
        const std::thread::id owner{std::this_thread::get_id()};
    };

    namespace {
        Result<std::shared_ptr<NativeServiceModule>> FindModule(NativeExtensionServiceState &host, const std::string_view extension,
                                                                const std::string_view id) {
            if (host.owner != std::this_thread::get_id())
                return Result<std::shared_ptr<NativeServiceModule>>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
            if (host.closed.load(std::memory_order_acquire))
                return Result<std::shared_ptr<NativeServiceModule>>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
            for (const auto &weak : host.modules) {
                const auto module = weak.lock();
                if (module && !module->revoked && module->committed && module->extension == extension && module->declaration.id == id)
                    return Result<std::shared_ptr<NativeServiceModule>>::Success(module);
            }
            return Result<std::shared_ptr<NativeServiceModule>>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        }

        Result<ScriptCapabilityImport> ScriptImport(NativeServiceModule &module, const NativeServicePublication &publication) {
            if (!module.admission || !module.admission->ActivationLease().IsUsable())
                return Result<ScriptCapabilityImport>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
            const auto providerOwner = publication.owner.lock();
            if (!providerOwner || providerOwner->revoked || !providerOwner->admission)
                return Result<ScriptCapabilityImport>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
            auto grant = providerOwner->admission->Grant({publication.shape.service});
            if (grant.HasError())
                return Result<ScriptCapabilityImport>::Failure(grant.ErrorValue());
            const auto &descriptor = *publication.descriptors->Find(publication.shape.scriptApi);
            ScriptCapabilityImport import{descriptor.id,
                                          descriptor.compatibility,
                                          publication.descriptors,
                                          std::move(grant).Value(),
                                          publication.script->Lease(),
                                          ScriptCapabilityProfile::Tooling,
                                          {}};
            for (const auto &method : publication.shape.methods)
                import.functions.push_back(method.id);
            return Result<ScriptCapabilityImport>::Success(std::move(import));
        }

        bool ScriptSessionUsable(const NativeServiceModule &module, const ScriptCapabilityBinding &binding,
                                 const NativeServicePublication &publication) noexcept {
            const auto host = module.host.lock();
            return host && !host->closed.load(std::memory_order_acquire) && module.committed.load(std::memory_order_acquire) &&
                   !module.revoked.load(std::memory_order_acquire) && publication.script.has_value() && binding.IsUsable();
        }

        bool InvocationShape(const std::string_view function, const std::span<const std::int64_t> arguments) noexcept {
            return arguments.size() <= HORO_EXTENSION_SERVICE_MAX_ARGUMENTS && function.size() <= 256;
        }

        struct CallCancellation final {
            CancellationToken caller;
            CancellationToken invocation;
        };

        std::uint8_t CallCancelled(const void *opaque) {
            const auto &value = *static_cast<const CallCancellation *>(opaque);
            return value.caller.IsCancellationRequested() || value.invocation.IsCancellationRequested();
        }

        Result<void> Finish(ScriptInvocationController &controller, const Result<std::int64_t> &result) {
            auto observed = controller.ObserveCancellation();
            if (observed.HasError())
                return Result<void>::Failure(observed.ErrorValue());
            if (result.HasError()) {
                auto failed = controller.Fail(result.ErrorValue());
                if (failed.HasError())
                    return Result<void>::Failure(failed.ErrorValue());
            } else {
                auto completed = controller.Complete(ScriptCallResult::Success({ScriptValue{result.Value()}}));
                if (completed.HasError())
                    return Result<void>::Failure(completed.ErrorValue());
            }
            const auto snapshot = controller.Handle().Snapshot();
            if (!snapshot || snapshot->state == ScriptInvocationStateKind::Cancelled)
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationCancelled));
            return Result<void>::Success();
        }
    }  // namespace

    Result<std::shared_ptr<NativeExtensionScriptSession>> NativeExtensionServiceHost::BindScript(const std::string_view extension,
                                                                                                 const std::string_view moduleId,
                                                                                                 const std::string_view importId,
                                                                                                 ScriptCapabilityContextDescriptor policy,
                                                                                                 const CancellationToken &cancellation) {
        auto moduleResult = FindModule(*state_, extension, moduleId);
        if (moduleResult.HasError())
            return Result<std::shared_ptr<NativeExtensionScriptSession>>::Failure(moduleResult.ErrorValue());
        auto module = std::move(moduleResult).Value();
        const auto found = std::ranges::find(module->imports, importId, &ResolvedExtensionServiceImport::ImportId);
        if (found == module->imports.end())
            return Result<std::shared_ptr<NativeExtensionScriptSession>>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        auto selected = FindNativeService(*state_, *found);
        if (selected.HasError())
            return Result<std::shared_ptr<NativeExtensionScriptSession>>::Failure(selected.ErrorValue());
        auto publication = std::move(selected).Value();
        if (!publication->script || !publication->descriptors || !policy.imports.empty())
            return Result<std::shared_ptr<NativeExtensionScriptSession>>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        auto import = ScriptImport(*module, *publication);
        if (import.HasError())
            return Result<std::shared_ptr<NativeExtensionScriptSession>>::Failure(import.ErrorValue());
        policy.imports.push_back(std::move(import).Value());
        auto contextResult = ScriptCapabilityContext::Create(state_->invocations, std::move(policy), {.parentCancellation = cancellation});
        if (contextResult.HasError())
            return Result<std::shared_ptr<NativeExtensionScriptSession>>::Failure(contextResult.ErrorValue());
        auto context = std::move(contextResult).Value();
        auto binding = context.Bind(publication->shape.scriptApi);
        if (binding.HasError())
            return Result<std::shared_ptr<NativeExtensionScriptSession>>::Failure(binding.ErrorValue());
        auto impl = std::make_unique<NativeExtensionScriptSession::Impl>(module, publication, *found, std::move(context),
                                                                         std::move(binding).Value());
        return Result<std::shared_ptr<NativeExtensionScriptSession>>::Success(
            std::shared_ptr<NativeExtensionScriptSession>{new NativeExtensionScriptSession{std::move(impl)}});
    }

    NativeExtensionScriptSession::NativeExtensionScriptSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    NativeExtensionScriptSession::~NativeExtensionScriptSession() {
        Reset();
    }

    std::span<const ScriptExportFunctionDescriptor> NativeExtensionScriptSession::Functions() const noexcept {
        return impl_->publication->descriptors->Find(impl_->publication->shape.scriptApi)->functions;
    }

    void NativeExtensionScriptSession::Reset() noexcept {
        impl_->context.Reset();
    }

    Result<std::int64_t> NativeExtensionScriptSession::Invoke(const std::string_view function,
                                                              const std::span<const std::int64_t> arguments,
                                                              const CancellationToken &cancellation) {
        if (impl_->owner != std::this_thread::get_id())
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!ScriptSessionUsable(*impl_->module, impl_->binding, *impl_->publication))
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        if (!InvocationShape(function, arguments))
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        ScriptInvocationRequest request{.functionId = std::string{function}};
        for (const auto argument : arguments)
            request.arguments.emplace_back(argument);
        auto started = impl_->binding.Begin(*impl_->publication->script, std::move(request));
        if (started.HasError())
            return Result<std::int64_t>::Failure(started.ErrorValue());
        auto controller = std::move(started).Value();
        CallCancellation cancelled{cancellation, controller.Cancellation()};
        auto result = InvokeNativeServiceImport(*impl_->module, impl_->import, {function, arguments, {&cancelled, CallCancelled}},
                                                controller.Cancellation());
        if (CallCancelled(&cancelled))
            static_cast<void>(controller.Handle().RequestCancellation());
        auto completed = Finish(controller, result);
        auto delivered = impl_->context.Drain(64);
        if (completed.HasError())
            return Result<std::int64_t>::Failure(completed.ErrorValue());
        if (delivered.HasError())
            return Result<std::int64_t>::Failure(delivered.ErrorValue());
        return result;
    }
}  // namespace Horo::Extensions
