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
            static const std::array<ErrorCodeDescriptor, 4> errors{
                {{ErrorDomainId{"horo.extensions.native-service"}, ErrorCode{"invalid_call"}, ErrorSeverity::Error,
                  "Native service call is invalid."},
                 {ErrorDomainId{"horo.extensions.native-service"}, ErrorCode{"overflow"}, ErrorSeverity::Error,
                  "Native service arithmetic overflow."},
                 {ErrorDomainId{"horo.extensions.native-service"}, ErrorCode{"requested_failure"}, ErrorSeverity::Error,
                  "Native service requested failure."},
                 {ErrorDomainId{"horo.extensions.native-service"}, ErrorCode{"retired"}, ErrorSeverity::Error, "Native service retired."}}};
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
                                             std::shared_ptr<ExtensionModuleLifetime> module)
        : shape_(std::move(shape)), provider_(native.provider), invoke_(native.invoke), shutdown_(native.shutdown),
          module_(std::move(module)) {}

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
        for (auto &publication : std::ranges::reverse_view(publications))
            publication->Revoke();
    }

    Result<std::shared_ptr<NativeServicePublication>> FindNativeService(const NativeExtensionServiceState &host,
                                                                        const ResolvedExtensionServiceImport &import) {
        for (const auto &weak : host.modules) {
            const auto module = weak.lock();
            if (!module || !module->committed || module->revoked || module->declaration.id != import.ProviderModuleId())
                continue;
            for (const auto &publication : module->publications)
                if (publication->shape.service == import.ServiceId() && publication->shape.contract == import.ContractId())
                    return Result<std::shared_ptr<NativeServicePublication>>::Success(publication);
        }
        return Result<std::shared_ptr<NativeServicePublication>>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
    }

    Result<std::int64_t> InvokeNativeServiceImport(NativeServiceModule &module, const ResolvedExtensionServiceImport &import,
                                                   const NativeServiceCall &request, const CancellationToken &cancel) {
        if (module.owner != std::this_thread::get_id())
            return Failed("Native service selection crossed its owner lane.");
        const auto host = module.host.lock();
        if (!host)
            return Failed("Native service host expired.");
        auto selected = FindNativeService(*host, import);
        if (selected.HasError())
            return Result<std::int64_t>::Failure(selected.ErrorValue());
        return InvokeSelectedNativeServiceImport(module, import, *selected.Value(), request, cancel);
    }

    Result<std::int64_t> InvokeSelectedNativeServiceImport(NativeServiceModule &module, const ResolvedExtensionServiceImport &import,
                                                           const NativeServicePublication &publication, const NativeServiceCall &request,
                                                           const CancellationToken &cancel) {
        if (!module.committed.load(std::memory_order_acquire) || module.revoked.load(std::memory_order_acquire))
            return Failed("Native service caller activation is closed.");
        const auto host = module.host.lock();
        if (!host || host->closed.load(std::memory_order_acquire) || !module.admission)
            return Failed("Native service host is closed.");
        auto grant = module.admission->Grant({import.ServiceId()});
        if (grant.HasError())
            return Result<std::int64_t>::Failure(grant.ErrorValue());
        const auto generation = module.admission->ActivationLease().Activation().Generation();
        auto provider = host->capabilities->ResolveExact(grant.Value(), {publication.shape.version, publication.shape.version},
                                                         publication.provider, module.extension, module.declaration.id, generation);
        if (provider.HasError())
            return Result<std::int64_t>::Failure(provider.ErrorValue());
        auto binding = host->services->BindImport(std::move(provider).Value(), import);
        if (binding.HasError())
            return Result<std::int64_t>::Failure(binding.ErrorValue());
        auto call = host->services->ResolveImported<NativeScalarService>(std::move(binding).Value());
        if (call.HasError())
            return Result<std::int64_t>::Failure(call.ErrorValue());
        return std::move(call).Value().Invoke(&NativeScalarService::Invoke, request, cancel);
    }
}  // namespace Horo::Extensions
