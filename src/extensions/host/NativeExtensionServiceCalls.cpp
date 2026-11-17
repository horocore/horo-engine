#include "NativeExtensionServiceInternal.h"

#include <array>
#include <cstring>

namespace Horo::Extensions {
    namespace {
        struct Cancellation final {
            const BackendServiceCallContext &backend;
            HoroExtensionCancellation caller;
        };

        std::uint8_t Cancelled(const void *opaque) {
            const auto &value = *static_cast<const Cancellation *>(opaque);
            return value.backend.IsCancellationRequested() ||
                   (value.caller.isCancellationRequested && value.caller.isCancellationRequested(value.caller.context));
        }

        Result<std::int64_t> Failed(const std::string_view message) {
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::InvocationFailed, std::string{message}));
        }

        bool ResultText(const HoroExtensionServiceResult &result) noexcept {
            if (result.structSize != sizeof(result) || result.errorLength > sizeof(result.error) || result.reserved != 0 ||
                result.failureCode >= HORO_EXTENSION_SERVICE_FAILURE_COUNT)
                return false;
            return std::ranges::all_of(std::span{result.error}.first(result.errorLength), [](unsigned char c) {
                return c >= 32 && c < 127;
            });
        }

        Result<std::int64_t> ProviderFailed(const HoroExtensionServiceResult &result) {
            using enum Horo::ErrorSeverity;
            static const std::array<ErrorCodeDescriptor, 4> errors{
                {{ErrorDomainId{"horo.extensions.native-service"}, ErrorCode{"invalid_call"}, Error, "Native service call is invalid."},
                 {ErrorDomainId{"horo.extensions.native-service"}, ErrorCode{"overflow"}, Error, "Native service arithmetic overflow."},
                 {ErrorDomainId{"horo.extensions.native-service"}, ErrorCode{"requested_failure"}, Error,
                  "Native service requested failure."},
                 {ErrorDomainId{"horo.extensions.native-service"}, ErrorCode{"retired"}, Error, "Native service retired."}}};
            if (result.failureCode == 0)
                return Failed("Native service failed without a semantic failure code.");
            return Result<std::int64_t>::Failure(MakeError(errors[result.failureCode - 1], std::string{result.error, result.errorLength}));
        }

        bool ValidCall(const NativeServiceCall &call, const NativeServiceShape &shape) {
            const auto method = std::ranges::find(shape.methods, call.method, &NativeServiceMethod::id);
            return method != shape.methods.end() && call.arguments.size() == method->arguments;
        }
    }  // namespace

    NativeScalarService::NativeScalarService(NativeServiceShape shape, const HoroExtensionServiceDescriptor &native,
                                             std::shared_ptr<ExtensionModuleLifetime> moduleLifetime)
        : shape_(std::move(shape)), provider_(native.provider), invoke_(native.invoke), shutdown_(native.shutdown),
          module_(std::move(moduleLifetime)) {}

    Result<std::int64_t> NativeScalarService::Invoke(const NativeServiceCall &request, const BackendServiceCallContext &context) const {
        if (!ValidCall(request, shape_))
            return Failed("Native service method or signed-integer argument shape is invalid.");
        Cancellation cancellation{context, request.nativeCancellation};
        if (Cancelled(&cancellation))
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::BackendServiceCancelled));
        HoroExtensionServiceRequest nativeRequest{.structSize = sizeof(nativeRequest),
                                                  .method = {request.method.data(), static_cast<std::uint32_t>(request.method.size())},
                                                  .arguments = request.arguments.data(),
                                                  .argumentCount = static_cast<std::uint32_t>(request.arguments.size()),
                                                  .cancellation = {&cancellation, Cancelled}};
        HoroExtensionServiceResult result{.structSize = sizeof(result)};
        const auto status = invoke_(provider_, &nativeRequest, &result);
        if (Cancelled(&cancellation) || status == HORO_EXTENSION_ERROR_CANCELLED)
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::BackendServiceCancelled));
        if (!ResultText(result))
            return Failed("Native service returned malformed bounded error evidence.");
        if (status != HORO_EXTENSION_SUCCESS)
            return ProviderFailed(result);
        if (result.errorLength != 0 || result.failureCode != 0)
            return Failed("Native service success cannot contain failure text.");
        return Result<std::int64_t>::Success(result.value);
    }

    NativeScalarService::~NativeScalarService() {
        Shutdown();
    }

    void NativeScalarService::Shutdown() noexcept {
        const auto shutdown = std::exchange(shutdown_, nullptr);
        if (!shutdown)
            return;
        try {
            shutdown(provider_);
        } catch (...) {
            if (const auto retirement = module_->retirement.lock())
                retirement->RequireRestart();
        }
    }

    void NativeServicePublication::Revoke() noexcept {
        if (capability)
            capability->Reset();
        if (script)
            script->Reset();
        if (service)
            static_cast<void>(service->Reset());
        service.reset();
        script.reset();
        capability.reset();
        pending.reset();
    }

    void NativeServiceModule::Revoke() noexcept {
        revoked = true;
        committed = false;
        if (admission)
            admission->Revoke();
        if (owner != std::this_thread::get_id()) {
            if (const auto native = retirement.lock())
                native->RequireRestart();
            return;  // Actual retirement retains native owners; mutable publication tables are owner-only.
        }
        for (const auto &publication : std::ranges::reverse_view(publications))
            publication->Revoke();
    }

    Result<std::shared_ptr<NativeServicePublication>> FindNativeService(const NativeExtensionServiceState &host,
                                                                        const ResolvedExtensionServiceImport &resolvedImport) {
        for (const auto &weak : host.modules) {
            const auto nativeModule = weak.lock();
            if (!nativeModule || !nativeModule->committed || nativeModule->revoked ||
                nativeModule->declaration.id != resolvedImport.ProviderModuleId())
                continue;
            for (const auto &publication : nativeModule->publications)
                if (publication->shape.service == resolvedImport.ServiceId() && publication->shape.contract == resolvedImport.ContractId())
                    return Result<std::shared_ptr<NativeServicePublication>>::Success(publication);
        }
        return Result<std::shared_ptr<NativeServicePublication>>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
    }

    Result<std::int64_t> InvokeNativeServiceImport(NativeServiceModule &nativeModule, const ResolvedExtensionServiceImport &resolvedImport,
                                                   const NativeServiceCall &request, const CancellationToken &cancel) {
        if (nativeModule.owner != std::this_thread::get_id())
            return Failed("Native service selection crossed its owner lane.");
        const auto host = nativeModule.host.lock();
        if (!host)
            return Failed("Native service host expired.");
        auto selected = FindNativeService(*host, resolvedImport);
        if (selected.HasError())
            return Result<std::int64_t>::Failure(selected.ErrorValue());
        return InvokeSelectedNativeServiceImport(nativeModule, resolvedImport, *selected.Value(), request, cancel);
    }

    Result<std::int64_t> InvokeSelectedNativeServiceImport(NativeServiceModule &nativeModule,
                                                           const ResolvedExtensionServiceImport &resolvedImport,
                                                           const NativeServicePublication &publication, const NativeServiceCall &request,
                                                           const CancellationToken &cancel) {
        if (!nativeModule.committed.load(std::memory_order_acquire) || nativeModule.revoked.load(std::memory_order_acquire))
            return Failed("Native service caller activation is closed.");
        const auto host = nativeModule.host.lock();
        if (!host || host->closed.load(std::memory_order_acquire) || !nativeModule.admission)
            return Failed("Native service host is closed.");
        auto grant = nativeModule.admission->Grant({resolvedImport.ServiceId()});
        if (grant.HasError())
            return Result<std::int64_t>::Failure(grant.ErrorValue());
        const auto generation = nativeModule.admission->ActivationLease().Activation().Generation();
        auto provider =
            host->capabilities->ResolveExact(grant.Value(), {publication.shape.version, publication.shape.version}, publication.provider,
                                             nativeModule.extension, nativeModule.declaration.id, generation);
        if (provider.HasError())
            return Result<std::int64_t>::Failure(provider.ErrorValue());
        auto binding = host->services->BindImport(std::move(provider).Value(), resolvedImport);
        if (binding.HasError())
            return Result<std::int64_t>::Failure(binding.ErrorValue());
        auto call = host->services->ResolveImported<NativeScalarService>(std::move(binding).Value());
        if (call.HasError())
            return Result<std::int64_t>::Failure(call.ErrorValue());
        return std::move(call).Value().Invoke(&NativeScalarService::Invoke, request, cancel);
    }
}  // namespace Horo::Extensions

namespace Horo::Extensions {
    namespace {
        std::uint8_t ScriptCallCancelled(const void *opaque) {
            return static_cast<const BackendServiceCallContext *>(opaque)->IsCancellationRequested();
        }
    }  // namespace

    Result<ScriptCallResult> NativeScalarService::InvokeScript(const ScriptInvocationRequest &request,
                                                               const BackendServiceCallContext &context) const {
        using Output = Result<ScriptCallResult>;
        if (request.arguments.size() > HORO_EXTENSION_SERVICE_MAX_ARGUMENTS)
            return Output::Failure(MakeError(ExtensionErrors::ScriptValueTypeMismatch));
        std::array<std::int64_t, HORO_EXTENSION_SERVICE_MAX_ARGUMENTS> arguments{};
        for (std::size_t index = 0; index < request.arguments.size(); ++index) {
            const auto *integer = request.arguments[index].AsSignedInteger();
            if (!integer)
                return Output::Failure(MakeError(ExtensionErrors::ScriptValueTypeMismatch));
            arguments[index] = *integer;
        }
        auto native =
            Invoke({request.functionId, std::span{arguments}.first(request.arguments.size()), {&context, ScriptCallCancelled}}, context);
        if (native.HasError())
            return Output::Failure(native.ErrorValue());
        return Output::Success(ScriptCallResult::Success({ScriptValue{native.Value()}}));
    }
}  // namespace Horo::Extensions
