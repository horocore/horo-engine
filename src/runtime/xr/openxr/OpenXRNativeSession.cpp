#include "host/OpenXRNativeSession.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Keeps native phase/result evidence bounded and private to adapter error translation. */
        Error NativeError(const char *operation, XrResult result) {
            const auto &descriptor = result == XR_ERROR_RUNTIME_UNAVAILABLE       ? XRErrors::RuntimeUnavailable
                                     : result == XR_ERROR_FORM_FACTOR_UNAVAILABLE ? XRErrors::SystemTemporarilyUnavailable
                                     : result == XR_ERROR_FORM_FACTOR_UNSUPPORTED ? XRErrors::SystemUnsupported
                                     : result == XR_ERROR_EXTENSION_NOT_PRESENT || result == XR_ERROR_API_LAYER_NOT_PRESENT
                                         ? XRErrors::OperationUnsupported
                                         : XRErrors::OperationUnavailable;
            return MakeError(descriptor, std::string{operation} + " failed; native result=" + std::to_string(result));
        }

        /** @brief Resolves exactly one candidate-scoped official function; absence never becomes success. */
        template <typename Function>
        Result<void> Resolve(PFN_xrGetInstanceProcAddr getProc, XrInstance instance, const char *name, Function &destination) {
            PFN_xrVoidFunction function{};
            const XrResult result = getProc(instance, name, &function);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeError(name, result));
            if (!function)
                return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, std::string{name} + " dispatch is absent"));
            destination = reinterpret_cast<Function>(function);
            return Result<void>::Success();
        }

        /** @brief Rejects empty, embedded-NUL, oversized or non-ASCII native policy names before copying. */
        bool ValidName(std::string_view name, std::size_t capacity) noexcept {
            return !name.empty() && name.size() < capacity && std::ranges::all_of(name, [](unsigned char character) {
                return character >= 33 && character <= 126;
            });
        }

        /** @brief Reads a native string only within the supplied fixed ABI storage. */
        template <std::size_t Size> std::string_view NativeName(const char (&name)[Size]) noexcept {
            const auto end = std::find(name, name + Size, '\0');
            return end == name + Size ? std::string_view{} : std::string_view{name, static_cast<std::size_t>(end - name)};
        }

        /** @brief Validates a selected API's binding tag without importing any platform graphics headers. */
        bool MatchesGraphicsBinding(std::string_view extension, XrStructureType type) noexcept {
            struct Binding {
                std::string_view extension;
                XrStructureType type;
            };

            constexpr std::array bindings{Binding{"XR_KHR_vulkan_enable", XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR},
                                          Binding{"XR_KHR_vulkan_enable2", XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR},
                                          Binding{"XR_KHR_opengl_enable", XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR},
                                          Binding{"XR_KHR_opengl_enable", XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR},
                                          Binding{"XR_KHR_opengl_enable", XR_TYPE_GRAPHICS_BINDING_OPENGL_XCB_KHR},
                                          Binding{"XR_KHR_opengl_enable", XR_TYPE_GRAPHICS_BINDING_OPENGL_WAYLAND_KHR},
                                          Binding{"XR_KHR_opengl_es_enable", XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR},
                                          Binding{"XR_KHR_D3D11_enable", XR_TYPE_GRAPHICS_BINDING_D3D11_KHR},
                                          Binding{"XR_KHR_D3D12_enable", XR_TYPE_GRAPHICS_BINDING_D3D12_KHR},
                                          Binding{"XR_KHR_metal_enable", XR_TYPE_GRAPHICS_BINDING_METAL_KHR}};
            return std::ranges::any_of(bindings, [extension, type](const Binding &binding) {
                return binding.extension == extension && binding.type == type;
            });
        }

        /** @brief Checks the complete fixed-storage extension publication contract before merging it. */
        bool ValidExtensionPublication(const XrExtensionProperties &entry) noexcept {
            return entry.type == XR_TYPE_EXTENSION_PROPERTIES && !entry.next &&
                   ValidName(NativeName(entry.extensionName), XR_MAX_EXTENSION_NAME_SIZE) && entry.extensionVersion != 0;
        }

        /** @brief Forms a bounded union of runtime and admitted-layer extensions without accepting malformed entries. */
        Result<void> MergeExtensions(std::span<const XrExtensionProperties> publication,
                                     std::array<XrExtensionProperties, MaximumNativeExtensions> &available, std::uint32_t &availableCount) {
            for (const auto &entry : publication) {
                if (!ValidExtensionPublication(entry))
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Malformed native extension publication"));
                const auto name = NativeName(entry.extensionName);
                const auto end = available.begin() + availableCount;
                if (std::find_if(available.begin(), end, [name](const auto &extension) {
                    return NativeName(extension.extensionName) == name;
                }) != end)
                    continue;  // Global and enabled layers may advertise the same extension.
                if (availableCount == available.size())
                    return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
                available[availableCount++] = entry;
            }
            return Result<void>::Success();
        }

        /** @brief Rejects malformed layer metadata even when that layer was not requested. */
        bool ValidLayerPublication(const XrApiLayerProperties &layer) noexcept {
            return layer.type == XR_TYPE_API_LAYER_PROPERTIES && !layer.next &&
                   ValidName(NativeName(layer.layerName), XR_MAX_API_LAYER_NAME_SIZE);
        }

        /** @brief Reads only the bounded host-binding extension contract; no graphics API is selected here. */
        Result<std::string_view> GraphicsExtensionName(const char *extension) {
            if (!extension)
                return Result<std::string_view>::Failure(MakeError(XRErrors::OperationUnsupported, "Explicit graphics binding is absent"));
            std::size_t length{};
            while (length < XR_MAX_EXTENSION_NAME_SIZE && extension[length] != '\0')
                ++length;
            const std::string_view name{extension, length};
            if (!ValidName(name, XR_MAX_EXTENSION_NAME_SIZE))
                return Result<std::string_view>::Failure(MakeError(XRErrors::OperationInvalid));
            return Result<std::string_view>::Success(name);
        }

        /** @brief Validates extension policy names and uniqueness independently of runtime availability. */
        Result<void> ValidateExtensionRequests(std::span<const NativeExtensionRequest> requests, std::string_view bindingName) {
            for (std::size_t index = 0; index < requests.size(); ++index) {
                const auto name = requests[index].name;
                if (!ValidName(name, XR_MAX_EXTENSION_NAME_SIZE))
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
                if (name == bindingName)
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Graphics extension is already host-owned"));
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (requests[prior].name == name)
                        return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Duplicate native extension request"));
            }
            return Result<void>::Success();
        }

        /** @brief Preserves the feature-plan admission category at the native activation boundary. */
        Error FeaturePlanError(XRFeatureNegotiationStatus status) {
            switch (status) {
                case XRFeatureNegotiationStatus::StaleSystem:
                    return MakeError(XRErrors::IdentityStale);
                case XRFeatureNegotiationStatus::StaleRevision:
                    return MakeError(XRErrors::CapabilityStale);
                case XRFeatureNegotiationStatus::CapacityExceeded:
                    return MakeError(XRErrors::CapacityExceeded);
                case XRFeatureNegotiationStatus::Unavailable:
                    return MakeError(XRErrors::OperationUnavailable);
                default:
                    return MakeError(XRErrors::OperationInvalid);
            }
        }
    }  // namespace

    /** @copydoc OpenXRNativeSession::OpenXRNativeSession */
    OpenXRNativeSession::OpenXRNativeSession(std::shared_ptr<const Platform::DynamicLibrary> loader, IOpenXRGraphicsBinding &graphics,
                                             const IOpenXRCompositionFence &fence) noexcept
        : loader_(std::move(loader)), graphics_(&graphics), fence_(&fence) {}

    /** @copydoc OpenXRNativeSession::~OpenXRNativeSession */
    OpenXRNativeSession::~OpenXRNativeSession() {
        const auto retired = Close();
        // Do not unload executable dispatch beneath an object whose destruction the runtime rejected.
        HORO_INVARIANT_MSG(retired.HasValue(), "OpenXR owner requires successful native retirement before destruction");
    }

    /** @copydoc OpenXRNativeSession::ResolveGlobal */
    Result<void> OpenXRNativeSession::ResolveGlobal() {
        if (!loader_)
            return Result<void>::Failure(MakeError(XRErrors::LoaderAbsent));
        getProc_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(loader_->GetSymbol("xrGetInstanceProcAddr"));
        if (!getProc_)
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Verified loader has no xrGetInstanceProcAddr"));
        // The official loader exports retirement as well as dispatch. Secure a
        // cleanup path before allocation, even if instance dispatch is corrupt.
        destroyInstance_ = reinterpret_cast<PFN_xrDestroyInstance>(loader_->GetSymbol("xrDestroyInstance"));
        if (!destroyInstance_)
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Verified loader has no bootstrap instance retirement"));
        auto resolved = Resolve(getProc_, XR_NULL_HANDLE, "xrEnumerateApiLayerProperties", enumerateLayers_);
        if (resolved.HasError())
            return resolved;
        resolved = Resolve(getProc_, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", enumerateExtensions_);
        if (resolved.HasError())
            return resolved;
        return Resolve(getProc_, XR_NULL_HANDLE, "xrCreateInstance", createInstance_);
    }

    /** @copydoc OpenXRNativeSession::CheckCurrent */
    Result<void> OpenXRNativeSession::CheckCurrent(const NativeSessionRequest &request) const {
        if (!request.candidate.IsValid() || request.preflight.RuntimeGeneration() != request.candidate.system.runtime)
            return Result<void>::Failure(MakeError(XRErrors::IdentityInvalid));
        auto admitted = ValidateXRLoaderPreflight(request.preflight, request.activeAttempt, request.preflight.Backend(),
                                                  request.preflight.InstallRecord(), request.preflight.ProductProfile());
        if (admitted.HasError())
            return admitted;
        const auto featureStatus =
            ValidateXRFeaturePlan(request.plan, request.capabilities, request.candidate.system, request.capabilities.Revision());
        if (featureStatus != XRFeatureNegotiationStatus::Ok)
            return Result<void>::Failure(FeaturePlanError(featureStatus));
        admitted = fence_->Validate(request.candidate, request.preflight, request.plan);
        return admitted.HasError() ? admitted : graphics_->Validate(request.candidate);
    }

    /** @copydoc OpenXRNativeSession::CollectExtensions */
    Result<void> OpenXRNativeSession::CollectExtensions(const char *layer,
                                                        std::array<XrExtensionProperties, MaximumNativeExtensions> &available,
                                                        std::uint32_t &availableCount) {
        std::array<XrExtensionProperties, MaximumNativeExtensions> publication{};
        for (auto &extension : publication)
            extension.type = XR_TYPE_EXTENSION_PROPERTIES;
        std::uint32_t count{};
        auto result = enumerateExtensions_(layer, 0, &count, nullptr);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrEnumerateInstanceExtensionProperties/count", result));
        if (count > publication.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        result = enumerateExtensions_(layer, static_cast<std::uint32_t>(publication.size()), &count, publication.data());
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrEnumerateInstanceExtensionProperties/list", result));
        if (count > publication.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        return MergeExtensions(std::span{publication.data(), count}, available, availableCount);
    }

    /** @copydoc OpenXRNativeSession::DiscoverAndNegotiate */
    Result<void> OpenXRNativeSession::DiscoverAndNegotiate(const NativeSessionRequest &request) {
        if (request.extensions.size() >= MaximumEnabledExtensions || request.layers.size() > MaximumNativeLayers)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (!request.layers.empty() && (!request.developmentLayersApproved || request.productMode != XRPreflightProductMode::Development))
            return Result<void>::Failure(MakeError(XRErrors::RuntimeOverrideRejected));
        const auto layers = DiscoverLayers(request.layers);
        return layers.HasError() ? layers : NegotiateExtensions(request.extensions);
    }

    /** @copydoc OpenXRNativeSession::DiscoverLayers */
    Result<void> OpenXRNativeSession::DiscoverLayers(std::span<const std::string_view> requested) {
        std::array<XrApiLayerProperties, MaximumNativeLayers> availableLayers{};
        for (auto &layer : availableLayers)
            layer.type = XR_TYPE_API_LAYER_PROPERTIES;
        std::uint32_t count{};
        XrResult result = enumerateLayers_(0, &count, nullptr);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrEnumerateApiLayerProperties/count", result));
        if (count > availableLayers.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        result = enumerateLayers_(static_cast<std::uint32_t>(availableLayers.size()), &count, availableLayers.data());
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrEnumerateApiLayerProperties/list", result));
        if (count > availableLayers.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const std::span publication{availableLayers.data(), count};
        if (!std::ranges::all_of(publication, ValidLayerPublication))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Malformed native layer publication"));
        layerCount_ = 0;
        for (const auto name : requested) {
            const auto admitted = EnableLayer(name, publication);
            if (admitted.HasError())
                return admitted;
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::EnableLayer */
    Result<void> OpenXRNativeSession::EnableLayer(std::string_view name, std::span<const XrApiLayerProperties> available) {
        if (!ValidName(name, XR_MAX_API_LAYER_NAME_SIZE))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (std::find(layers_.begin(), layers_.begin() + layerCount_, name) != layers_.begin() + layerCount_)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Duplicate API layer request"));
        if (std::ranges::none_of(available, [name](const auto &layer) {
            return NativeName(layer.layerName) == name;
        }))
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported, "Required API layer is absent"));
        if (layerCount_ == layerNames_.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        std::memcpy(layerNames_[layerCount_].data(), name.data(), name.size());
        layerNames_[layerCount_][name.size()] = '\0';
        layers_[layerCount_] = layerNames_[layerCount_].data();
        ++layerCount_;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::NegotiateExtensions */
    Result<void> OpenXRNativeSession::NegotiateExtensions(std::span<const NativeExtensionRequest> requested) {
        std::array<XrExtensionProperties, MaximumNativeExtensions> available{};
        std::uint32_t count{};
        auto collected = CollectExtensions(nullptr, available, count);
        if (collected.HasError())
            return collected;
        for (std::uint32_t index = 0; index < layerCount_; ++index) {
            collected = CollectExtensions(layers_[index], available, count);
            if (collected.HasError())
                return collected;
        }

        const auto binding = GraphicsExtensionName(graphics_->RequiredExtension());
        if (binding.HasError())
            return Result<void>::Failure(binding.ErrorValue());
        const auto bindingName = binding.Value();
        auto admitted = ValidateExtensionRequests(requested, bindingName);
        if (admitted.HasError())
            return admitted;
        extensionCount_ = 0;
        const std::span publication{available.data(), count};
        admitted = EnableExtension(bindingName, true, publication);
        if (admitted.HasError())
            return admitted;
        for (const auto &extension : requested) {
            admitted = EnableExtension(extension.name, extension.required, publication);
            if (admitted.HasError())
                return admitted;
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::EnableExtension */
    Result<void> OpenXRNativeSession::EnableExtension(std::string_view name, bool required,
                                                      std::span<const XrExtensionProperties> available) {
        if (std::ranges::none_of(available, [name](const auto &extension) {
            return NativeName(extension.extensionName) == name;
        }))
            return required ? Result<void>::Failure(MakeError(XRErrors::OperationUnsupported, "Required native extension is absent"))
                            : Result<void>::Success();
        if (extensionCount_ == extensionNames_.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        std::memcpy(extensionNames_[extensionCount_].data(), name.data(), name.size());
        extensionNames_[extensionCount_][name.size()] = '\0';
        extensions_[extensionCount_] = extensionNames_[extensionCount_].data();
        ++extensionCount_;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::CreateInstance */
    Result<void> OpenXRNativeSession::CreateInstance(const NativeSessionRequest &request) {
        const auto current = CheckCurrent(request);
        if (current.HasError())
            return current;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(info.applicationInfo.applicationName, "Horo Engine");
        std::strcpy(info.applicationInfo.engineName, "Horo Engine");
        const auto version = request.preflight.LoaderApiVersion();
        if (version.major != 1 || version.minor > XR_VERSION_MINOR(XR_CURRENT_API_VERSION))
            return Result<void>::Failure(
                MakeError(XRErrors::LoaderIncompatible, "Preflight API exceeds the privately compiled SDK contract"));
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(version.major, version.minor, version.patch);
        info.enabledApiLayerCount = layerCount_;
        info.enabledApiLayerNames = layers_.data();
        info.enabledExtensionCount = extensionCount_;
        info.enabledExtensionNames = extensions_.data();
        XrInstance candidate{XR_NULL_HANDLE};
        const auto result = createInstance_(&info, &candidate);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrCreateInstance", result));
        instance_ = candidate;
        if (instance_ == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "xrCreateInstance returned an empty success"));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::ResolveInstance */
    Result<void> OpenXRNativeSession::ResolveInstance() {
        auto resolved = Resolve(getProc_, instance_, "xrDestroyInstance", destroyInstance_);
        if (resolved.HasError())
            return resolved;
        resolved = Resolve(getProc_, instance_, "xrDestroySession", destroySession_);
        if (resolved.HasError())
            return resolved;
        resolved = Resolve(getProc_, instance_, "xrGetSystem", getSystem_);
        if (resolved.HasError())
            return resolved;
        return Resolve(getProc_, instance_, "xrCreateSession", createSession_);
    }

    /** @copydoc OpenXRNativeSession::SelectSystem */
    Result<void> OpenXRNativeSession::SelectSystem() {
        const XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
        const auto result = getSystem_(instance_, &info, &system_);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrGetSystem", result));
        return system_ != XR_NULL_SYSTEM_ID ? Result<void>::Success()
                                            : Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "xrGetSystem returned zero"));
    }

    /** @copydoc OpenXRNativeSession::CreateSession */
    Result<void> OpenXRNativeSession::CreateSession(const NativeSessionRequest &request) {
        auto current = CheckCurrent(request);
        if (current.HasError())
            return current;
        auto binding = graphics_->Prepare(instance_, system_, getProc_);
        if (binding.HasError())
            return Result<void>::Failure(binding.ErrorValue());
        graphicsPrepared_ = true;
        if (!binding.Value() || binding.Value()->next || !MatchesGraphicsBinding(extensions_[0], binding.Value()->type))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Graphics binding must be one explicit native structure"));
        current = CheckCurrent(request);
        if (current.HasError())
            return current;
        const XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO, binding.Value(), 0, system_};
        XrSession candidate{XR_NULL_HANDLE};
        const auto result = createSession_(instance_, &info, &candidate);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrCreateSession", result));
        session_ = candidate;
        return session_ != XR_NULL_HANDLE
                   ? Result<void>::Success()
                   : Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "xrCreateSession returned an empty success"));
    }

    /** @copydoc OpenXRNativeSession::Fail */
    Result<void> OpenXRNativeSession::Fail(Error error) {
        const auto retired = Close();
        return Result<void>::Failure(retired.HasError() ? WithCause(std::move(error), retired.ErrorValue()) : std::move(error));
    }

    /** @copydoc OpenXRNativeSession::Create */
    Result<void> OpenXRNativeSession::Create(const NativeSessionRequest &request) {
        if (instance_ != XR_NULL_HANDLE || session_ != XR_NULL_HANDLE || owner_.IsValid())
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnavailable, "Owner must retire before reuse; replacement uses a separate owner"));
        auto current = CheckCurrent(request);
        if (current.HasError())
            return current;
        if (request.candidate.system.runtime.Value() < lastRuntime_ ||
            (request.candidate.system.runtime.Value() == lastRuntime_ && request.candidate.slot.generation <= lastSessionGeneration_))
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        preparing_ = request.candidate;
        lastRuntime_ = request.candidate.system.runtime.Value();
        lastSessionGeneration_ = request.candidate.slot.generation;
        try {
            return PrepareTransaction(request);
        } catch (const std::exception &) {
            return Fail(MakeError(XRErrors::OperationUnavailable, "Host preparation threw; native candidate rollback requested"));
        } catch (...) {
            return Fail(MakeError(XRErrors::OperationUnavailable,
                                  "Host preparation raised an unknown exception; native candidate rollback requested"));
        }
    }

    /** @copydoc OpenXRNativeSession::PrepareTransaction */
    Result<void> OpenXRNativeSession::PrepareTransaction(const NativeSessionRequest &request) {
        auto prepared = ResolveGlobal();
        if (prepared.HasValue())
            prepared = DiscoverAndNegotiate(request);
        if (prepared.HasValue())
            prepared = CreateInstance(request);
        if (prepared.HasValue())
            prepared = ResolveInstance();
        if (prepared.HasValue())
            prepared = CheckCurrent(request);
        if (prepared.HasValue())
            prepared = SelectSystem();
        if (prepared.HasValue())
            prepared = CreateSession(request);
        if (prepared.HasValue())
            prepared = CheckCurrent(request);
        if (prepared.HasError())
            return Fail(prepared.ErrorValue());
        owner_ = request.candidate;
        retainedPreflight_ = request.preflight;
        retainedPlan_ = request.plan;
        preparing_ = {};
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::Close */
    Result<void> OpenXRNativeSession::Close() {
        owner_ = {};
        preparing_ = {};
        retainedPreflight_.reset();
        retainedPlan_.reset();
        if (session_ != XR_NULL_HANDLE) {
            if (!destroySession_)
                return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Native session has no retirement dispatch"));
            const auto result = destroySession_(session_);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeError("xrDestroySession", result));
            session_ = XR_NULL_HANDLE;
        }
        if (graphicsPrepared_) {
            graphics_->Release();
            graphicsPrepared_ = false;
        }
        if (instance_ != XR_NULL_HANDLE) {
            if (!destroyInstance_)
                return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Native instance has no retirement dispatch"));
            const auto result = destroyInstance_(instance_);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeError("xrDestroyInstance", result));
            instance_ = XR_NULL_HANDLE;
        }
        system_ = XR_NULL_SYSTEM_ID;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::Validate */
    Result<void> OpenXRNativeSession::Validate(const XRSessionId &session) const {
        const auto admitted = ValidateXRSession(session, owner_);
        if (admitted.HasError())
            return admitted;
        if (!retainedPreflight_ || !retainedPlan_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto current = fence_->Validate(session, *retainedPreflight_, *retainedPlan_);
        return current.HasError() ? current : graphics_->Validate(session);
    }

    /** @copydoc OpenXRNativeSession::Session */
    XRSessionId OpenXRNativeSession::Session() const noexcept {
        return owner_;
    }

    /** @copydoc OpenXRNativeSession::HasExtension */
    bool OpenXRNativeSession::HasExtension(std::string_view name) const noexcept {
        if (!owner_.IsValid())
            return false;
        for (std::uint32_t index = 0; index < extensionCount_; ++index)
            if (name == extensions_[index])
                return true;
        return false;
    }

    /** @copydoc OpenXRNativeSession::Borrow */
    Result<NativeSessionBorrow> OpenXRNativeSession::Borrow(const XRSessionId &session) const {
        const auto current = Validate(session);
        return current.HasError() ? Result<NativeSessionBorrow>::Failure(current.ErrorValue())
                                  : Result<NativeSessionBorrow>::Success({instance_, system_, session_});
    }
}  // namespace Horo::XR::OpenXRInternal
