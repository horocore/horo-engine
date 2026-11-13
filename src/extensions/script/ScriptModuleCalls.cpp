#include "ScriptModuleContextInternal.h"

#include <algorithm>

namespace Horo::Extensions {
    namespace {
        Result<void> ValidatePayload(const ScriptInvocationController &controller, const ScriptCallResult &result,
                                     const ScriptValueLimits &limits) {
            const auto *request = controller.Request();
            if (!request || !request->descriptors)
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
            const auto *api = request->descriptors->Find(request->apiId);
            if (!api)
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
            const auto function = std::ranges::find(api->functions, request->functionId, &ScriptExportFunctionDescriptor::id);
            if (function == api->functions.end())
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
            return ValidateScriptResult(*function, result, *api, limits);
        }

        Result<ScriptInvocationTransitionResult> Publish(ScriptInvocationController &controller, Result<ScriptCallResult> result,
                                                         const ScriptValueLimits &limits) {
            if (result.HasError())
                return controller.Fail(result.ErrorValue());
            auto payload = std::move(result).Value();
            if (auto valid = ValidatePayload(controller, payload, limits); valid.HasError())
                return Result<ScriptInvocationTransitionResult>::Failure(valid.ErrorValue());
            if (payload.IsFailure())
                return controller.Fail(std::move(*payload.error));
            return controller.Complete(std::move(payload));
        }

        void Finish(ScriptInvocationController &controller, Result<ScriptCallResult> result, const ScriptValueLimits &limits) {
            if (auto cancelled = controller.ObserveCancellation(); cancelled.HasError())
                throw cancelled.ErrorValue();
            auto terminal = Publish(controller, std::move(result), limits);
            if (terminal.HasError()) {
                // Invalid provider payload becomes the actual canonical validation error, never a false successful completion.
                auto failed = controller.Fail(terminal.ErrorValue());
                if (failed.HasError())
                    throw failed.ErrorValue();
            }
        }

        Result<ScriptCallResult> InvokeOwned(const ScriptRuntimeBackendBinding &backend, const ScriptInvocationRequest &request,
                                             const CancellationToken &cancel, const std::optional<ScriptModuleExecutionOwner> &source) {
            if (!source)
                return ScriptModuleContextAccess::Invoke(backend, request, cancel);
            const auto &owner = source->capability.Activation();
            if (auto use = source->capability.AcquireUse(owner.ExtensionId(), owner.ModuleId(), owner.Generation()); use.HasError())
                return Result<ScriptCallResult>::Failure(use.ErrorValue());
            else {
                // The actual use guard and copied executable lease span the complete backend member call.
                return ScriptModuleContextAccess::Invoke(backend, request, cancel);
            }
        }

        void Execute(std::shared_ptr<const ScriptRuntimeBackendBinding> backend, std::shared_ptr<ScriptInvocationController> controller,
                     ScriptInvocationRequest request, const CancellationToken &job, ScriptValueLimits limits,
                     std::optional<ScriptModuleExecutionOwner> source) {
            if (job.IsCancellationRequested())
                static_cast<void>(controller->Handle().RequestCancellation());
            if (controller->Cancellation().IsCancellationRequested()) {
                static_cast<void>(controller->ObserveCancellation());
                return;
            }
            try {
                Finish(*controller, InvokeOwned(*backend, request, controller->Cancellation(), source), limits);
            } catch (...) {
                static_cast<void>(controller->Fail(MakeError(ExtensionErrors::ScriptInvocationAbandoned)));
            }
        }

        Result<void> Defaults(std::vector<ScriptValue> &arguments, const ScriptExportFunctionDescriptor &function,
                              const ScriptValueLimits &limits) {
            for (std::size_t i = arguments.size(); i < function.parameters.size(); ++i) {
                const auto &parameter = function.parameters[i];
                if (!parameter.canonicalDefault)
                    break;
                auto value = DecodeScriptValue(*parameter.canonicalDefault, limits);
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                arguments.push_back(std::move(value).Value());
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<ScriptModuleContextAccess::ResolvedCall> ScriptModuleContextAccess::Resolve(ScriptModuleContext::Impl &state,
                                                                                       std::string_view alias, std::string_view function) {
        using Output = Result<ResolvedCall>;
        const auto entry = std::ranges::find(state.imports, alias, &ScriptModuleImport::Alias);
        if (entry == state.imports.end())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable, "Script import alias was not declared."));
        if (!entry->Backend())
            return Output::Failure(*entry->Unavailable());
        const auto bound = std::ranges::find(state.bound, entry->Backend(), &ScriptModuleContext::Impl::Bound::backend);
        if (bound == state.bound.end())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        const auto &api = bound->backend->Descriptor();
        const auto method = std::ranges::find(api.functions, function, &ScriptExportFunctionDescriptor::id);
        if (method == api.functions.end())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid, "Script function is not declared."));
        return Output::Success({&*bound, &*method});
    }

    Result<ScriptInvocationHandle> ScriptModuleContext::Invoke(std::string_view alias, std::string_view function,
                                                               std::vector<ScriptValue> arguments) {
        using Output = Result<ScriptInvocationHandle>;
        if (impl_->owner != std::this_thread::get_id())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!IsUsable())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
        ReapScriptModuleWork(*impl_);
        if (impl_->work.size() >= impl_->limits.maximumPendingCalls)
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded));
        auto selected = ScriptModuleContextAccess::Resolve(*impl_, alias, function);
        if (selected.HasError())
            return Output::Failure(selected.ErrorValue());
        const auto bound = selected.Value().bound;
        const auto method = selected.Value().function;
        if (auto defaults = Defaults(arguments, *method, impl_->limits.values); defaults.HasError())
            return Output::Failure(defaults.ErrorValue());
        if (auto argumentsValid = ValidateScriptArguments(*method, arguments, bound->backend->Descriptor(), impl_->limits.values);
            argumentsValid.HasError())
            return Output::Failure(argumentsValid.ErrorValue());
        ScriptInvocationRequest request{.functionId = std::string{function},
                                        .arguments = std::move(arguments),
                                        .timeout = impl_->limits.executionDeadline};
        auto admitted = bound->backend->Begin(bound->capability, std::move(request));
        if (admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        auto controller = std::make_shared<ScriptInvocationController>(std::move(admitted).Value());
        const auto handle = controller->Handle();
        auto copied = *controller->Request();
        if (method->invocation == ScriptExportInvocationMode::Synchronous) {
            Execute(bound->backend, std::move(controller), std::move(copied), {}, impl_->limits.values, impl_->executionOwner);
            return Output::Success(handle);
        }
        auto submitted =
            impl_->jobs.Submit({.parentCancellation = controller->Cancellation()},
                               [backend = bound->backend, controller, request = std::move(copied), limits = impl_->limits.values,
                                source = impl_->executionOwner](const CancellationToken &token) {
            Execute(backend, controller, request, token, limits, source);
        });
        if (submitted.HasError()) {
            static_cast<void>(controller->Fail(submitted.ErrorValue()));
            return Output::Success(handle);
        }
        impl_->work.push_back({std::move(submitted).Value(), handle, std::move(controller)});
        return Output::Success(handle);
    }

    Result<std::vector<ScriptInvocationEvent>> ScriptModuleContext::Poll() {
        if (impl_->owner != std::this_thread::get_id())
            return Result<std::vector<ScriptInvocationEvent>>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!IsUsable())
            return Result<std::vector<ScriptInvocationEvent>>::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
        for (const auto &work : impl_->work) {
            if (const auto job = work.job.Snapshot(); job && job->state == JobState::Cancelled)
                static_cast<void>(work.controller->Handle().RequestCancellation());
            static_cast<void>(work.controller->ObserveCancellation());
        }
        ReapScriptModuleWork(*impl_);
        return impl_->context.Drain(impl_->limits.maximumEventsPerPoll);
    }
}  // namespace Horo::Extensions
