#include "NativeExtensionServiceInternal.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Horo::Extensions {
    namespace {
        std::string_view View(const HoroExtensionStringView value) noexcept {
            return value.length == 0 ? std::string_view{} : std::string_view{value.data, value.length};
        }

        bool Text(const HoroExtensionStringView value) noexcept {
            return value.data && value.length > 0 && value.length <= 256;
        }

        HoroExtensionStatus Reject(AssetImporterRegistrationSession &load) noexcept {
            load.failed = true;
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }

        bool ScriptClaim(const AssetImporterRegistrationSession &load, const NativeServiceShape &shape) {
            if (shape.scriptApi.empty())
                return true;
            return std::ranges::any_of(load.manifest->contributions, [&](const auto &claim) {
                return claim.type == "script.api" && claim.id == shape.scriptApi && claim.owningModule == load.extensionModule->id;
            });
        }

        HoroExtensionStatus Register(AssetImporterRegistrationSession &load, const HoroExtensionServiceDescriptor &descriptor) {
            const auto &nativeModule = load.nativeServices;
            if (!nativeModule || !load.serviceTransportNegotiated || nativeModule->publications.size() >= 32)
                return Reject(load);
            auto shape = CopyNativeServiceShape(descriptor, *load.extensionModule);
            if (shape.HasError() || !ScriptClaim(load, shape.Value()))
                return Reject(load);
            for (const auto &previous : nativeModule->publications)
                if (previous->shape.service == shape.Value().service)
                    return Reject(load);
            auto publication = std::make_shared<NativeServicePublication>();
            publication->shape = std::move(shape).Value();
            publication->owner = nativeModule;
            publication->provider = {nativeModule->declaration.id, publication->shape.service,
                                     nativeModule->admission->ActivationLease().Activation().Generation()};
            if (!publication->shape.scriptApi.empty()) {
                auto scripts = NativeServiceScriptDescriptors(publication->shape, nativeModule->declaration.id);
                if (scripts.HasError())
                    return Reject(load);
                publication->descriptors = std::move(scripts).Value();
            }
            publication->pending = std::make_unique<NativeScalarService>(publication->shape, descriptor, load.lifetime);
            nativeModule->publications.push_back(std::move(publication));
            return HORO_EXTENSION_SUCCESS;
        }

        bool RequestValid(const HoroExtensionServiceRequest *request, const HoroExtensionServiceResult *result) noexcept {
            return request && result && result->structSize == sizeof(*result) && request->structSize == sizeof(*request) &&
                   Text(request->method) && request->argumentCount <= HORO_EXTENSION_SERVICE_MAX_ARGUMENTS &&
                   (request->argumentCount == 0 || request->arguments);
        }

        const Error &Leaf(const Error &error) noexcept {
            const auto *leaf = &error;
            for (unsigned depth = 0; depth < 16 && leaf->cause; ++depth)
                leaf = leaf->cause.Get();
            return *leaf;
        }

        std::uint32_t FailureCode(const Error &error) noexcept {
            if (error.domain.Value() != "horo.extensions.native-service")
                return HORO_EXTENSION_SERVICE_INVALID_CALL;
            static constexpr std::array<std::string_view, 4> codes{"invalid_call", "overflow", "requested_failure", "retired"};
            const auto found = std::ranges::find(codes, error.code.Value());
            return found == codes.end() ? HORO_EXTENSION_SERVICE_INVALID_CALL : static_cast<std::uint32_t>(found - codes.begin() + 1);
        }

        HoroExtensionStatus ProjectError(const Error &error, HoroExtensionServiceResult &result) noexcept {
            const auto &leaf = Leaf(error);
            const auto size = std::min(leaf.message.size(), sizeof(result.error));
            std::copy_n(leaf.message.begin(), size, result.error);
            result.errorLength = static_cast<std::uint32_t>(size);
            result.failureCode = FailureCode(leaf);
            if (ErrorChainContains(error, ExtensionErrors::BackendServiceCancelled.domain, ExtensionErrors::BackendServiceCancelled.code))
                return HORO_EXTENSION_ERROR_CANCELLED;
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }

        HoroExtensionStatus InvokeClient(void *opaque, const HoroExtensionServiceRequest *request,
                                         HoroExtensionServiceResult *result) noexcept {
            if (!opaque || !RequestValid(request, result))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            *result = {.structSize = sizeof(*result), .failureCode = HORO_EXTENSION_SERVICE_RETIRED};
            const auto &client = *static_cast<NativeServiceImportClient *>(opaque);
            const auto nativeModule = client.moduleState.lock();
            if (!nativeModule || !nativeModule->committed.load(std::memory_order_acquire))
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            try {
                const auto provider = client.provider.lock();
                if (!provider)
                    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
                auto value = InvokeSelectedNativeServiceImport(*nativeModule, client.resolvedImport, *provider,
                                                               {View(request->method),
                                                                {request->arguments, request->argumentCount},
                                                                request->cancellation},
                                                               {});
                if (value.HasError())
                    return ProjectError(value.ErrorValue(), *result);
                result->value = value.Value();
                result->errorLength = 0;
                result->failureCode = HORO_EXTENSION_SERVICE_NO_FAILURE;
                return HORO_EXTENSION_SUCCESS;
            } catch (...) {
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        HoroExtensionStatus Bind(AssetImporterRegistrationSession &load, const HoroExtensionStringView id,
                                 HoroExtensionServiceClient &output) {
            const auto &nativeModule = load.nativeServices;
            if (!nativeModule || !load.serviceTransportNegotiated || !Text(id) || nativeModule->clients.size() >= 64)
                return Reject(load);
            const auto found = std::ranges::find(nativeModule->imports, View(id), &ResolvedExtensionServiceImport::ImportId);
            if (found == nativeModule->imports.end() || found->Status() != ExtensionServiceImportStatus::Bound)
                return Reject(load);
            auto client = std::make_unique<NativeServiceImportClient>(nativeModule, *found);
            output = {sizeof(output), client.get(), InvokeClient};
            nativeModule->clients.push_back(std::move(client));
            return HORO_EXTENSION_SUCCESS;
        }
    }  // namespace

    HoroExtensionStatus RegisterNativeService(void *opaque, const HoroExtensionServiceDescriptor *descriptor) noexcept {
        if (!opaque || !descriptor)
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        auto &load = *static_cast<AssetImporterRegistrationSession *>(opaque);
        if (!load.lifetime || load.lifetime->ownerThread != std::this_thread::get_id())
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        try {
            return Register(load, *descriptor);
        } catch (...) {
            return Reject(load);
        }
    }

    HoroExtensionStatus BindNativeServiceImport(void *opaque, const HoroExtensionStringView id,
                                                HoroExtensionServiceClient *client) noexcept {
        if (!opaque || !client || client->structSize != sizeof(*client))
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        auto &load = *static_cast<AssetImporterRegistrationSession *>(opaque);
        if (!load.lifetime || load.lifetime->ownerThread != std::this_thread::get_id())
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        try {
            return Bind(load, id, *client);
        } catch (...) {
            return Reject(load);
        }
    }

    const HoroExtensionServiceHostApi &NativeServiceHostAbi() noexcept {
        static const HoroExtensionServiceHostApi api{sizeof(api), HORO_EXTENSION_SERVICE_SCHEMA_VERSION, RegisterNativeService,
                                                     BindNativeServiceImport};
        return api;
    }
}  // namespace Horo::Extensions
