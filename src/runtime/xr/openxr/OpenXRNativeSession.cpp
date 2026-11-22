#include "host/OpenXRNativeSession.h"

#include "host/OpenXRNativeNames.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <format>
#include <limits>
#include <string>

namespace Horo::XR::OpenXRInternal {
    /** @brief Cold allocation distinguishes destroyed and recreated owners even at the same address. */
    struct NativeFeatureProviderIssuer final {};

    namespace {
        /** @brief Maps native result categories without discarding phase or numeric evidence. */
        const ErrorCodeDescriptor &NativeErrorDescriptor(XrResult result) noexcept {
            switch (result) {
                case XR_ERROR_RUNTIME_UNAVAILABLE:
                    return XRErrors::RuntimeUnavailable;
                case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
                    return XRErrors::SystemTemporarilyUnavailable;
                case XR_ERROR_FORM_FACTOR_UNSUPPORTED:
                    return XRErrors::SystemUnsupported;
                case XR_ERROR_PERMISSION_INSUFFICIENT:
                    return XRErrors::PermissionDenied;
                case XR_ERROR_EXTENSION_NOT_PRESENT:
                case XR_ERROR_API_LAYER_NOT_PRESENT:
                    return XRErrors::OperationUnsupported;
                default:
                    return XRErrors::OperationUnavailable;
            }
        }

        /** @brief Keeps native phase/result evidence bounded and private to adapter error translation. */
        Error NativeError(const char *operation, XrResult result) {
            return MakeError(NativeErrorDescriptor(result),
                             std::format("{} failed; native result={}", operation, static_cast<std::int32_t>(result)));
        }

        /** @brief Fixed outcome of host invocation; error allocation and native retirement stay outside this boundary. */
        enum class PreparationFailure {
            None,
            StandardException,
            UnknownException
        };

        /** @brief Contains every host preparation exception without allocating a translated error while unwinding. */
        template <typename Prepare> PreparationFailure CapturePreparationFailure(Prepare prepare) noexcept {
            using enum PreparationFailure;
            try {
                prepare();
                return None;
            } catch (const std::exception &) {
                return StandardException;
            } catch (...) {
                return UnknownException;
            }
        }

        /** @brief Resolves exactly one candidate-scoped official function; absence never becomes success. */
        template <typename GetProc, typename Function>
        Result<void> Resolve(GetProc getProc, XrInstance instance, const char *name, Function &destination) {
            PFN_xrVoidFunction function{};
            const XrResult result = getProc(instance, name, &function);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeError(name, result));
            if (!function)
                return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, std::string{name} + " dispatch is absent"));
            // OpenXR dispatch preserves the native function-pointer representation; bit_cast enforces equal size.
            destination = std::bit_cast<Function>(function);
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

        /** @brief Copies a validated policy name into bounded owner storage, including its explicit terminator. */
        bool CopyPolicyName(std::string_view name, std::span<char> destination) noexcept {
            if (!ValidName(name, destination.size()))
                return false;
            std::ranges::copy(name, destination.first(name.size()).begin());
            destination[name.size()] = '\0';
            return true;
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
                if (const auto previous = std::find_if(available.begin(), end,
                                                       [name](const auto &extension) {
                    return NativeName(extension.extensionName) == name;
                });
                    previous != end) {
                    previous->extensionVersion = std::max(previous->extensionVersion, entry.extensionVersion);
                    continue;
                }
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
                const auto &request = requests[index];
                if (!ValidName(name, XR_MAX_EXTENSION_NAME_SIZE) || request.minimumVersion == 0 ||
                    (request.promotedToCore && !request.promotedToCore->IsValid()) ||
                    request.eligibility > NativeExtensionEligibility::Denied)
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
                if (request.dependencies.size() >= MaximumEnabledExtensions)
                    return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
                if (std::ranges::any_of(request.dependencies, [](const std::string_view dependency) {
                    return !ValidName(dependency, XR_MAX_EXTENSION_NAME_SIZE);
                }))
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Malformed native extension dependency"));
                if (name == bindingName)
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Graphics extension is already host-owned"));
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (requests[prior].name == name)
                        return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Duplicate native extension request"));
            }
            return Result<void>::Success();
        }

        /** @brief Reads the highest version from the validated union of runtime and admitted layer records. */
        std::uint32_t DiscoveredVersion(std::span<const XrExtensionProperties> available, const std::string_view name) noexcept {
            const auto found = std::ranges::find(available, name, [](const XrExtensionProperties &extension) {
                return NativeName(extension.extensionName);
            });
            return found == available.end() ? 0 : found->extensionVersion;
        }

        /** @brief Classifies version and core availability before resolving dependencies. */
        NativeExtensionState VersionAdmission(const NativeExtensionRequest &request, const std::uint32_t discovered,
                                              const XRLoaderApiVersion core) noexcept {
            if (request.eligibility == NativeExtensionEligibility::Denied)
                return NativeExtensionState::Denied;
            if (request.eligibility == NativeExtensionEligibility::Unsupported)
                return NativeExtensionState::Unsupported;
            if (request.promotedToCore && core >= *request.promotedToCore)
                return NativeExtensionState::PromotedToCore;
            if (discovered == 0)
                return NativeExtensionState::Absent;
            return discovered >= request.minimumVersion ? NativeExtensionState::Enabled : NativeExtensionState::VersionIncompatible;
        }

        /** @brief Holds startup-only memoized dependency resolution state for one requested feature. */
        struct PlannedExtension final {
            NativeExtensionAdmission evidence;
            bool decided{};
            bool visiting{};
            bool requiredForFallback{};
        };

        /** @brief Checks whether either the exact extension or its declared core route was admitted. */
        bool Available(NativeExtensionState state) noexcept {
            return state == NativeExtensionState::Enabled || state == NativeExtensionState::PromotedToCore;
        }

        /** @brief Resolves only host-requested dependencies. A repeated active index cannot admit a cycle. */
        bool ResolveDependencies(std::span<const NativeExtensionRequest> requests, std::span<PlannedExtension> plan, std::size_t index,
                                 std::string_view graphics) noexcept {
            auto &entry = plan[index];
            if (entry.decided)
                return Available(entry.evidence.state);
            if (entry.visiting)
                return false;
            entry.visiting = true;
            if (Available(entry.evidence.state)) {
                for (const auto name : requests[index].dependencies) {
                    if (name == graphics)
                        continue;
                    const auto found = std::ranges::find(requests, name, &NativeExtensionRequest::name);
                    if (found == requests.end() ||
                        !ResolveDependencies(requests, plan, static_cast<std::size_t>(found - requests.begin()), graphics)) {
                        entry.evidence.state = NativeExtensionState::DependencyMissing;
                        break;
                    }
                }
            }
            entry.decided = true;
            return Available(entry.evidence.state);
        }

        /** @brief Preserves the dependency closure of one required admitted route for baseline retry. */
        void KeepRequiredDependencies(std::span<const NativeExtensionRequest> requests, std::span<PlannedExtension> plan,
                                      std::size_t index) noexcept {
            auto &entry = plan[index];
            if (entry.requiredForFallback)
                return;
            entry.requiredForFallback = true;
            for (const auto name : requests[index].dependencies) {
                const auto found = std::ranges::find(requests, name, &NativeExtensionRequest::name);
                if (found != requests.end())
                    KeepRequiredDependencies(requests, plan, static_cast<std::size_t>(found - requests.begin()));
            }
        }

        /** @brief Rechecks core dependencies against the reduced extension set without mutating live evidence. */
        void PlanExtensionFallback(std::span<const NativeExtensionRequest> requests, std::span<PlannedExtension> plan,
                                   std::string_view graphics) noexcept {
            for (std::size_t index = 0; index < requests.size(); ++index)
                if (requests[index].required)
                    KeepRequiredDependencies(requests, plan, index);
            for (auto &entry : plan) {
                entry.decided = false;
                entry.visiting = false;
                if (entry.evidence.state == NativeExtensionState::Enabled && !entry.requiredForFallback)
                    entry.evidence.state = NativeExtensionState::ProviderFailed;
            }
            for (std::size_t index = 0; index < plan.size(); ++index)
                static_cast<void>(ResolveDependencies(requests, plan, index, graphics));
        }

        /** @brief Preserves distinct reasons for required extension rejection before native instance creation. */
        Error ExtensionRejection(NativeExtensionState state) {
            switch (state) {
                case NativeExtensionState::VersionIncompatible:
                    return MakeError(XRErrors::OperationIncompatible, "Native extension version is insufficient");
                case NativeExtensionState::DependencyMissing:
                    return MakeError(XRErrors::OperationUnavailable, "Native extension dependencies are unavailable");
                case NativeExtensionState::Denied:
                    return MakeError(XRErrors::PermissionDenied, "Native extension is denied by the host");
                case NativeExtensionState::Unsupported:
                    return MakeError(XRErrors::OperationUnsupported, "Native extension is unsupported by the selected host");
                case NativeExtensionState::Discovered:
                    return MakeError(XRErrors::OperationUnsupported, "Native extension was discovered without provider admission");
                case NativeExtensionState::ProviderFailed:
                    return MakeError(XRErrors::OperationUnavailable, "Optional native extension was omitted after an instance refusal");
                default:
                    return MakeError(XRErrors::OperationUnsupported, "Native extension is absent");
            }
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

#if defined(__ANDROID__)
    /** @copydoc OpenXRNativeSession::OpenXRNativeSession(std::shared_ptr<const Platform::DynamicLibrary>, IOpenXRGraphicsBinding &, const
     * IOpenXRCompositionFence &, NativeOpenXRAndroidApplication) */
    OpenXRNativeSession::OpenXRNativeSession(std::shared_ptr<const Platform::DynamicLibrary> loader, IOpenXRGraphicsBinding &graphics,
                                             const IOpenXRCompositionFence &fence, NativeOpenXRAndroidApplication application) noexcept
        : androidApplication_(std::move(application)), loader_(std::move(loader)), graphics_(&graphics), fence_(&fence) {}

    /** @copydoc OpenXRNativeSession::InitializeAndroidLoader */
    Result<void> OpenXRNativeSession::InitializeAndroidLoader(std::span<const NativeExtensionRequest> requested) {
        const auto &application = androidApplication_;
        if (!application.vm || !application.applicationContext || !application.activity || !application.lifetime)
            return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightInvalid,
                                                   "Selected Android host has no retained OpenXR application context and Activity"));
        if (requested.size() >= MaximumEnabledExtensions)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (!std::ranges::any_of(requested, [](const NativeExtensionRequest &extension) {
            return extension.name == AndroidOpenXRInstanceExtension && extension.required && !extension.promotedToCore;
        }))
            return Result<void>::Failure(
                MakeError(XRErrors::LoaderPreflightInvalid, "Android OpenXR selection must require XR_KHR_android_create_instance"));

        const auto initialized = InitializeAndroidOpenXRLoader(dispatch_.getProc, application);
        if (initialized == XR_ERROR_FUNCTION_UNSUPPORTED)
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Selected loader has no Android initialization dispatch"));
        return initialized == XR_SUCCESS ? Result<void>::Success()
                                         : Result<void>::Failure(NativeError("xrInitializeLoaderKHR", initialized));
    }
#endif

    /** @copydoc OpenXRNativeSession::~OpenXRNativeSession */
    OpenXRNativeSession::~OpenXRNativeSession() {
        const auto retired = Close();
        // Do not unload executable dispatch beneath an object whose destruction the runtime rejected.
        HORO_INVARIANT_MSG(retired.HasValue(), "OpenXR owner requires successful native retirement before destruction");
    }

    /** @copydoc OpenXRNativeSession::ResolveGlobal */
    Result<void> OpenXRNativeSession::ResolveGlobal([[maybe_unused]] const NativeDiscoveryRequest &request) {
        if (!loader_)
            return Result<void>::Failure(MakeError(XRErrors::LoaderAbsent));
        dispatch_.getProc = std::bit_cast<PFN_xrGetInstanceProcAddr>(loader_->GetSymbol("xrGetInstanceProcAddr"));
        if (!dispatch_.getProc)
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Verified loader has no xrGetInstanceProcAddr"));
        // The official loader exports retirement as well as dispatch. Secure a
        // cleanup path before allocation, even if instance dispatch is corrupt.
        dispatch_.destroyInstance = std::bit_cast<PFN_xrDestroyInstance>(loader_->GetSymbol("xrDestroyInstance"));
        if (!dispatch_.destroyInstance)
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Verified loader has no bootstrap instance retirement"));
#if defined(__ANDROID__)
        if (auto initialized = InitializeAndroidLoader(request.extensions); initialized.HasError())
            return initialized;
#endif
        auto resolved = Resolve(dispatch_.getProc, XR_NULL_HANDLE, "xrEnumerateApiLayerProperties", dispatch_.enumerateLayers);
        if (resolved.HasError())
            return resolved;
        resolved = Resolve(dispatch_.getProc, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", dispatch_.enumerateExtensions);
        if (resolved.HasError())
            return resolved;
        return Resolve(dispatch_.getProc, XR_NULL_HANDLE, "xrCreateInstance", dispatch_.createInstance);
    }

    /** @copydoc OpenXRNativeSession::CheckCurrent */
    Result<void> OpenXRNativeSession::CheckCurrent(const NativeSessionRequest &request) const {
        if (controlThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR native owner requires its control thread"));
        if (!request.candidate.IsValid() || request.preflight.RuntimeGeneration() != request.candidate.system.runtime)
            return Result<void>::Failure(MakeError(XRErrors::IdentityInvalid));
        if (request.preflight.ProductMode() != request.productMode)
            return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightStale));
        auto admitted = ValidateXRLoaderPreflight(request.preflight, request.activeAttempt, request.preflight.Backend(),
                                                  request.preflight.InstallRecord(), request.preflight.ProductProfile());
        if (admitted.HasError())
            return admitted;
        if (const auto featureStatus =
                ValidateXRFeaturePlan(request.plan, request.capabilities, request.candidate.system, request.capabilities.Revision());
            featureStatus != XRFeatureNegotiationStatus::Ok)
            return Result<void>::Failure(FeaturePlanError(featureStatus));
        admitted = fence_->Validate(request.candidate, request.preflight, request.plan);
        return admitted.HasError() ? admitted : graphics_->Validate(request.candidate);
    }

    /** @copydoc OpenXRNativeSession::CollectExtensions */
    Result<void> OpenXRNativeSession::CollectExtensions(const char *layer,
                                                        std::array<XrExtensionProperties, MaximumNativeExtensions> &available,
                                                        std::uint32_t &availableCount) const {
        std::array<XrExtensionProperties, MaximumNativeExtensions> publication{};
        for (auto &extension : publication)
            extension.type = XR_TYPE_EXTENSION_PROPERTIES;
        std::uint32_t count{};
        auto result = dispatch_.enumerateExtensions(layer, 0, &count, nullptr);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrEnumerateInstanceExtensionProperties/count", result));
        if (count > publication.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        result = dispatch_.enumerateExtensions(layer, static_cast<std::uint32_t>(publication.size()), &count, publication.data());
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrEnumerateInstanceExtensionProperties/list", result));
        if (count > publication.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        return MergeExtensions(std::span{publication.data(), count}, available, availableCount);
    }

    /** @copydoc OpenXRNativeSession::DiscoverAndNegotiate */
    Result<void> OpenXRNativeSession::DiscoverAndNegotiate(const NativeDiscoveryRequest &request) {
        if (request.extensions.size() >= MaximumEnabledExtensions || request.layers.size() > MaximumNativeLayers)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (!request.layers.empty() &&
            (!request.developmentLayersApproved || request.selection.productMode != XRPreflightProductMode::Development))
            return Result<void>::Failure(MakeError(XRErrors::RuntimeOverrideRejected));
        const auto layers = DiscoverLayers(request.layers);
        if (layers.HasError())
            return layers;
        if (!request.optionalReprojection)
            return NegotiateExtensions(request.extensions);

        const auto reprojection = ReprojectionExtensionRequests();
        if (request.extensions.size() + reprojection.size() >= MaximumEnabledExtensions)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        // Baseline routes may never depend on extensions the same instance preparation can omit.
        for (const auto &extension : request.extensions)
            for (const auto dependency : extension.dependencies)
                if (std::ranges::find(reprojection, dependency, &NativeExtensionRequest::name) != reprojection.end())
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        std::array<NativeExtensionRequest, MaximumEnabledExtensions> extensions{};
        const auto copied = std::ranges::copy(request.extensions, extensions.begin());
        const auto end = std::ranges::copy(reprojection, copied.out).out;
        return NegotiateExtensions(std::span{extensions.begin(), end});
    }

    /** @copydoc OpenXRNativeSession::DiscoverLayers */
    Result<void> OpenXRNativeSession::DiscoverLayers(std::span<const std::string_view> requested) {
        std::array<XrApiLayerProperties, MaximumNativeLayers> availableLayers{};
        for (auto &layer : availableLayers)
            layer.type = XR_TYPE_API_LAYER_PROPERTIES;
        std::uint32_t count{};
        XrResult result = dispatch_.enumerateLayers(0, &count, nullptr);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrEnumerateApiLayerProperties/count", result));
        if (count > availableLayers.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        result = dispatch_.enumerateLayers(static_cast<std::uint32_t>(availableLayers.size()), &count, availableLayers.data());
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrEnumerateApiLayerProperties/list", result));
        if (count > availableLayers.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const std::span publication{availableLayers.data(), count};
        if (!std::ranges::all_of(publication, ValidLayerPublication))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Malformed native layer publication"));
        names_.layerCount = 0;
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
        if (std::find(names_.layers.begin(), names_.layers.begin() + names_.layerCount, name) != names_.layers.begin() + names_.layerCount)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Duplicate API layer request"));
        if (std::ranges::none_of(available, [name](const auto &layer) {
            return NativeName(layer.layerName) == name;
        }))
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported, "Required API layer is absent"));
        if (names_.layerCount == names_.layerNames.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (!CopyPolicyName(name, names_.layerNames[names_.layerCount]))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        names_.layers[names_.layerCount] = names_.layerNames[names_.layerCount].data();
        ++names_.layerCount;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::NegotiateExtensions */
    Result<void> OpenXRNativeSession::NegotiateExtensions(std::span<const NativeExtensionRequest> requested) {
        discoveredExtensionCount_ = 0;
        extensionEvidenceCount_ = 0;
        auto collected = CollectExtensions(nullptr, discoveredExtensions_, discoveredExtensionCount_);
        if (collected.HasError())
            return collected;
        for (std::uint32_t index = 0; index < names_.layerCount; ++index) {
            collected = CollectExtensions(names_.layers[index], discoveredExtensions_, discoveredExtensionCount_);
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
        names_.extensionCount = 0;
        const std::span publication{discoveredExtensions_.data(), discoveredExtensionCount_};
        if (DiscoveredVersion(publication, bindingName) == 0)
            return Result<void>::Failure(ExtensionRejection(NativeExtensionState::Absent));
        admitted = EnableExtension(bindingName);
        if (admitted.HasError())
            return admitted;
        return AdmitRequestedExtensions(requested, bindingName, publication);
    }

    /** @copydoc OpenXRNativeSession::AdmitRequestedExtensions */
    Result<void> OpenXRNativeSession::AdmitRequestedExtensions(std::span<const NativeExtensionRequest> requested,
                                                               std::string_view bindingName,
                                                               std::span<const XrExtensionProperties> publication) {
        std::array<PlannedExtension, MaximumEnabledExtensions> plan{};
        for (std::size_t index = 0; index < requested.size(); ++index) {
            const auto &request = requested[index];
            const auto discovered = DiscoveredVersion(publication, request.name);
            const auto state = VersionAdmission(request, discovered, discovery_->acceptedApi);
            plan[index].evidence = {state, discovered, request.minimumVersion,
                                    state == NativeExtensionState::PromotedToCore ? std::optional{discovery_->acceptedApi} : std::nullopt};
        }
        const auto entries = std::span{plan}.first(requested.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto &request = requested[index];
            if (!ResolveDependencies(requested, entries, index, bindingName) && request.required)
                return Result<void>::Failure(ExtensionRejection(entries[index].evidence.state));
            auto &record = extensionEvidence_[extensionEvidenceCount_++];
            if (!CopyPolicyName(request.name, record.name))
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
            record.admission = entries[index].evidence;
            if (record.admission.state != NativeExtensionState::PromotedToCore)
                record.admission.coreVersion.reset();
            if (record.admission.state == NativeExtensionState::Enabled) {
                if (auto enabled = EnableExtension(request.name); enabled.HasError())
                    return enabled;
            }
        }
        PlanExtensionFallback(requested, entries, bindingName);
        for (std::size_t index = 0; index < entries.size(); ++index)
            extensionEvidence_[index].fallbackState = entries[index].evidence.state;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::EnableExtension */
    Result<void> OpenXRNativeSession::EnableExtension(std::string_view name) {
        if (names_.extensionCount == names_.extensionNames.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (!CopyPolicyName(name, names_.extensionNames[names_.extensionCount]))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        names_.extensions[names_.extensionCount] = names_.extensionNames[names_.extensionCount].data();
        ++names_.extensionCount;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::OmitOptionalExtensions */
    bool OpenXRNativeSession::OmitOptionalExtensions() noexcept {
        const auto enabled = std::span{names_.extensions}.first(names_.extensionCount);
        const auto omitted = std::ranges::remove_if(enabled, [&](std::string_view name) {
            const auto slot = ExtensionSlot(name);
            return slot < extensionEvidenceCount_ && extensionEvidence_[slot].fallbackState != NativeExtensionState::Enabled;
        });
        if (omitted.empty())
            return false;
        names_.extensionCount = static_cast<std::uint32_t>(omitted.begin() - enabled.begin());
        for (auto &record : std::span{extensionEvidence_}.first(extensionEvidenceCount_)) {
            record.admission.state = record.fallbackState;
            if (record.fallbackState != NativeExtensionState::PromotedToCore)
                record.admission.coreVersion.reset();
        }
        return true;
    }

    /** @copydoc OpenXRNativeSession::CreateInstance */
    Result<void> OpenXRNativeSession::CreateInstance(const NativeDiscoveryRequest &request) {
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        CopyNativeLiteral(info.applicationInfo.applicationName, "Horo Engine");
        CopyNativeLiteral(info.applicationInfo.engineName, "Horo Engine");
        const auto version = request.requestedApi;
        if (version.major != 1 || version.minor > XR_VERSION_MINOR(XR_CURRENT_API_VERSION))
            return Result<void>::Failure(
                MakeError(XRErrors::LoaderIncompatible, "Preflight API exceeds the privately compiled SDK contract"));
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(version.major, version.minor, version.patch);
        info.enabledApiLayerCount = names_.layerCount;
        info.enabledApiLayerNames = names_.layers.data();
        info.enabledExtensionCount = names_.extensionCount;
        info.enabledExtensionNames = names_.extensions.data();
#if defined(__ANDROID__)
        const auto slot = ExtensionSlot(AndroidOpenXRInstanceExtension);
        if (slot == extensionEvidenceCount_ || extensionEvidence_[slot].admission.state != NativeExtensionState::Enabled)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported, "Android OpenXR instance extension was not enabled"));
#endif
        XrInstance candidate{XR_NULL_HANDLE};
        const auto create = [&] {
#if defined(__ANDROID__)
            return CreateAndroidOpenXRInstance(dispatch_.createInstance, info, androidApplication_, candidate);
#else
            return dispatch_.createInstance(&info, &candidate);
#endif
        };
        auto result = create();
        if (result == XR_ERROR_EXTENSION_NOT_PRESENT && candidate == XR_NULL_HANDLE && OmitOptionalExtensions()) {
            info.enabledExtensionCount = names_.extensionCount;
            result = create();
        }
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrCreateInstance", result));
        instance_ = candidate;
        if (instance_ == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "xrCreateInstance returned an empty success"));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::ResolveInstance */
    Result<void> OpenXRNativeSession::ResolveInstance() {
        auto resolved = Resolve(dispatch_.getProc, instance_, "xrDestroyInstance", dispatch_.destroyInstance);
        if (resolved.HasError())
            return resolved;
        resolved = Resolve(dispatch_.getProc, instance_, "xrDestroySession", dispatch_.destroySession);
        if (resolved.HasError())
            return resolved;
        resolved = Resolve(dispatch_.getProc, instance_, "xrGetSystem", dispatch_.getSystem);
        if (resolved.HasError())
            return resolved;
        return Resolve(dispatch_.getProc, instance_, "xrCreateSession", dispatch_.createSession);
    }

    /** @copydoc OpenXRNativeSession::SelectSystem */
    Result<void> OpenXRNativeSession::SelectSystem() {
        const XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
        const auto result = dispatch_.getSystem(instance_, &info, &system_);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrGetSystem", result));
        return system_ != XR_NULL_SYSTEM_ID ? Result<void>::Success()
                                            : Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "xrGetSystem returned zero"));
    }

    /** @copydoc OpenXRNativeSession::CreateSession */
    Result<void> OpenXRNativeSession::CreateSession(const NativeSessionPreparation &request) {
        auto current = CheckPreparation(request);
        if (current.HasError())
            return current;
        if (nativeSessionIncarnation_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable, "Native session creation receipt exhausted"));
        auto binding = graphics_->Prepare(instance_, system_, {dispatch_.getProc});
        if (binding.HasError())
            return Result<void>::Failure(binding.ErrorValue());
        graphicsPrepared_ = true;
        if (!binding.Value() || binding.Value()->next || !MatchesGraphicsBinding(names_.extensions[0], binding.Value()->type))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Graphics binding must be one explicit native structure"));
        current = CheckPreparation(request);
        if (current.HasError())
            return current;
        const XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO, binding.Value(), 0, system_};
        XrSession candidate{XR_NULL_HANDLE};
        const auto result = dispatch_.createSession(instance_, &info, &candidate);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeError("xrCreateSession", result));
        session_ = candidate;
        if (session_ != XR_NULL_HANDLE)
            ++nativeSessionIncarnation_;
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
        if (controlThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR native owner requires its control thread"));
        if (discovery_ || instance_ != XR_NULL_HANDLE || session_ != XR_NULL_HANDLE || owner_.IsValid())
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnavailable, "Owner must retire before reuse; replacement uses a separate owner"));
        if (const auto current = CheckCurrent(request); current.HasError())
            return current;
        if (request.candidate.system.runtime.Value() < lastRuntime_ ||
            (request.candidate.system.runtime.Value() == lastRuntime_ && request.candidate.slot.generation <= lastSessionGeneration_))
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        preparing_ = request.candidate;
        lastRuntime_ = request.candidate.system.runtime.Value();
        lastSessionGeneration_ = request.candidate.slot.generation;
        auto prepared = Result<void>::Success();
        const auto failure = CapturePreparationFailure([this, &prepared, &request] {
            prepared = PrepareTransaction(request);
        });
        using enum PreparationFailure;
        if (failure == StandardException)
            return Fail(MakeError(XRErrors::OperationUnavailable, "Host preparation threw; native candidate rollback requested"));
        if (failure == UnknownException)
            return Fail(MakeError(XRErrors::OperationUnavailable,
                                  "Host preparation raised an unknown exception; native candidate rollback requested"));
        return prepared;
    }

    /** @copydoc OpenXRNativeSession::PrepareTransaction */
    Result<void> OpenXRNativeSession::PrepareTransaction(const NativeSessionRequest &request) {
        const auto api = request.preflight.LoaderApiVersion();
        const NativeDiscoveryRequest discovery{.selection = {.contractVersion = request.preflight.ContractVersion(),
                                                             .attempt = request.activeAttempt,
                                                             .backend = request.preflight.Backend(),
                                                             .installRecord = request.preflight.InstallRecord(),
                                                             .productProfile = request.preflight.ProductProfile(),
                                                             .loaderSource = request.preflight.LoaderSource(),
                                                             .productMode = request.productMode,
                                                             .runtimeSelection = request.preflight.RuntimeSelection(),
                                                             .admittedLoaderVersions = {api, api},
                                                             .developerOverrideApproved =
                                                                 request.preflight.RuntimeSelection() ==
                                                                 XRRuntimeSelectionPolicy::ApprovedDeveloperOverride},
                                               .requestedApi = api,
                                               .extensions = request.extensions,
                                               .layers = request.layers,
                                               .developmentLayersApproved = request.developmentLayersApproved};
        auto prepared = PrepareDiscovery(discovery);
        if (prepared.HasValue())
            prepared = PrepareSession({request.preflight, request.activeAttempt});
        return prepared.HasError() ? FinishCreate(std::move(prepared)) : FinishCreate(Activate(request));
    }

    /** @copydoc OpenXRNativeSession::FinishCreate */
    Result<void> OpenXRNativeSession::FinishCreate(Result<void> outcome) {
        if (outcome.HasError() && discovery_ && discovery_->phase != DiscoveryPhase::Retiring)
            return Fail(std::move(outcome).ErrorValue());
        // A retiring owner retains failed-destroy debt. Only a later explicit Close may retry native destruction.
        return outcome;
    }

    /** @copydoc OpenXRNativeSession::NativeFailure */
    Error OpenXRNativeSession::NativeFailure(const char *operation, const XrResult result) {
        return NativeError(operation, result);
    }

    /** @copydoc OpenXRNativeSession::Validate */
    Result<void> OpenXRNativeSession::Validate(const XRSessionId &session) const {
        if (const auto admitted = ValidateXRSession(session, owner_); admitted.HasError())
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
        if (controlThread_ != std::this_thread::get_id() || !owner_.IsValid())
            return false;
        for (std::uint32_t index = 0; index < names_.extensionCount; ++index)
            if (name == names_.extensions[index])
                return true;
        return false;
    }

    /** @copydoc OpenXRNativeSession::ReadExtensionAdmission */
    Result<NativeExtensionAdmission> OpenXRNativeSession::ReadExtensionAdmission(const XRLoaderPreflightAttempt attempt,
                                                                                 const std::string_view name) const {
        using Admission = Result<NativeExtensionAdmission>;
        if (controlThread_ != std::this_thread::get_id())
            return Admission::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!discovery_ || discovery_->selection.attempt != attempt || instance_ == XR_NULL_HANDLE || system_ == XR_NULL_SYSTEM_ID ||
            (discovery_->phase != DiscoveryPhase::Prepared && discovery_->phase != DiscoveryPhase::SessionPrepared &&
             discovery_->phase != DiscoveryPhase::Active))
            return Admission::Failure(MakeError(XRErrors::LoaderPreflightStale));
        if (!ValidName(name, XR_MAX_EXTENSION_NAME_SIZE))
            return Admission::Failure(MakeError(XRErrors::OperationInvalid));
        const auto slot = ExtensionSlot(name);
        if (slot < extensionEvidenceCount_)
            return Admission::Success(extensionEvidence_[slot].admission);
        const auto discovered = DiscoveredVersion({discoveredExtensions_.data(), discoveredExtensionCount_}, name);
        const auto native = std::span{names_.extensions}.first(names_.extensionCount);
        const auto enabled = std::ranges::find(native, name, [](const char *extension) {
            return std::string_view{extension};
        }) != native.end();
        return Admission::Success(
            {enabled ? NativeExtensionState::Enabled : (discovered ? NativeExtensionState::Discovered : NativeExtensionState::Absent),
             discovered, enabled ? 1U : 0U});
    }

    /** @copydoc OpenXRNativeSession::ReadExtensionAdmission */
    Result<NativeExtensionAdmission> OpenXRNativeSession::ReadExtensionAdmission(const XRSessionId &session,
                                                                                 const std::string_view name) const {
        using Outcome = Result<NativeExtensionAdmission>;
        if (controlThread_ != std::this_thread::get_id())
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (auto current = Validate(session); current.HasError())
            return Outcome::Failure(std::move(current).ErrorValue());
        if (owner_ != session || !discovery_)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        auto admission = ReadExtensionAdmission(discovery_->selection.attempt, name);
        const auto slot = ExtensionSlot(name);
        if (admission.HasError() || slot == extensionEvidenceCount_ || !featureProviders_[slot].failed)
            return admission;
        auto failed = std::move(admission).Value();
        failed.state = NativeExtensionState::ProviderFailed;
        return Result<NativeExtensionAdmission>::Success(failed);
    }

    /** @copydoc OpenXRNativeSession::ExtensionSlot */
    std::size_t OpenXRNativeSession::ExtensionSlot(const std::string_view name) const noexcept {
        const auto evidence = std::span{extensionEvidence_}.first(extensionEvidenceCount_);
        const auto found = std::ranges::find(evidence, name, [](const ExtensionEvidence &entry) {
            return std::string_view{entry.name.data()};
        });
        return static_cast<std::size_t>(found - evidence.begin());
    }

    /** @copydoc OpenXRNativeSession::RegisterFeatureProvider */
    Result<NativeFeatureProviderHandle> OpenXRNativeSession::RegisterFeatureProvider(const XRSessionId &session,
                                                                                     const NativeFeatureProviderRoute route) {
        return RegisterFeatureProvider(session, route, std::nullopt);
    }

    /** @copydoc OpenXRNativeSession::RegisterFeatureProvider */
    Result<NativeFeatureProviderHandle> OpenXRNativeSession::RegisterFeatureProvider(
        const XRSessionId &session, const NativeFeatureProviderRoute route, const std::optional<PreparedFeatureDispatch> prepared) {
        using Outcome = Result<NativeFeatureProviderHandle>;
        if (controlThread_ != std::this_thread::get_id() || registeringProvider_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!ValidName(route.extension, XR_MAX_EXTENSION_NAME_SIZE))
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        const auto slot = ExtensionSlot(route.extension);
        if (slot == extensionEvidenceCount_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported, "Native feature provider has no host-admitted route"));
        const auto admission = extensionEvidence_[slot].admission;
        if (!Available(admission.state))
            return Outcome::Failure(ExtensionRejection(admission.state));
        if (featureProviders_[slot].generation != 0)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible, "Native feature provider is already registered"));
        if (providerGeneration_ == std::numeric_limits<std::uint64_t>::max())
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        std::array<char, XR_MAX_EXTENSION_NAME_SIZE> symbol{};
        if (!CopyPolicyName(admission.coreVersion ? route.coreSymbol : route.extensionSymbol, symbol))
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Selected native feature provider has no valid symbol"));

        struct RegistrationScope final {
            bool &registering;

            explicit RegistrationScope(bool &active) noexcept : registering(active) {
                registering = true;
            }

            ~RegistrationScope() {
                registering = false;
            }
        };

        const RegistrationScope scope{registeringProvider_};
        const auto current = Borrow(session);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (owner_ != session || current.Value().instance == XR_NULL_HANDLE)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));

        if (prepared && prepared->incarnation != current.Value().incarnation)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));

        PFN_xrVoidFunction function = prepared ? prepared->function : nullptr;
        const auto result = prepared ? XR_SUCCESS : dispatch_.getProc(current.Value().instance, symbol.data(), &function);
        const auto fenced = Borrow(session);
        if (fenced.HasError())
            return Outcome::Failure(fenced.ErrorValue());
        if (owner_ != session || nativeSessionIncarnation_ != current.Value().incarnation)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const bool ready = result == XR_SUCCESS && function != nullptr;
        auto &provider = featureProviders_[slot];
        if (!ready) {
            provider.failed = true;
            return Outcome::Failure(result == XR_SUCCESS
                                        ? MakeError(XRErrors::OperationUnavailable, "Native provider returned an empty dispatch")
                                        : NativeFailure(symbol.data(), result));
        }
        if (!providerIssuer_)
            providerIssuer_ = std::make_shared<const NativeFeatureProviderIssuer>();
        provider = {function, ++providerGeneration_};
        return Outcome::Success({providerIssuer_, session, current.Value().incarnation, slot, provider.generation});
    }

    /** @copydoc OpenXRNativeSession::OwnsFeatureProvider */
    bool OpenXRNativeSession::OwnsFeatureProvider(const NativeFeatureProviderHandle &handle) const noexcept {
        return providerIssuer_ && !handle.issuer_.owner_before(providerIssuer_) && !providerIssuer_.owner_before(handle.issuer_) &&
               handle.session_ == owner_ && handle.incarnation_ == nativeSessionIncarnation_ && handle.slot_ < extensionEvidenceCount_ &&
               featureProviders_[handle.slot_].generation == handle.generation_;
    }

    /** @copydoc OpenXRNativeSession::BorrowFeatureProvider */
    Result<NativeFeatureProviderBorrow> OpenXRNativeSession::BorrowFeatureProvider(const NativeFeatureProviderHandle &handle) const {
        using Outcome = Result<NativeFeatureProviderBorrow>;
        if (controlThread_ != std::this_thread::get_id())
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!OwnsFeatureProvider(handle))
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        auto current = Borrow(handle.session_);
        if (current.HasError())
            return Outcome::Failure(std::move(current).ErrorValue());
        if (!OwnsFeatureProvider(handle))
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        return Outcome::Success({current.Value(), extensionEvidence_[handle.slot_].admission, featureProviders_[handle.slot_].function});
    }

    /** @copydoc OpenXRNativeSession::RevokeFeatureProvider */
    void OpenXRNativeSession::RevokeFeatureProvider(const NativeFeatureProviderHandle &handle) noexcept {
        if (controlThread_ == std::this_thread::get_id() && OwnsFeatureProvider(handle))
            featureProviders_[handle.slot_] = {};
    }

    /** @copydoc OpenXRNativeSession::ProjectionConfiguration */
    Result<NativeProjectionConfiguration> OpenXRNativeSession::ProjectionConfiguration(const XRSessionId &session) const {
        using Outcome = Result<NativeProjectionConfiguration>;
        if (const auto current = Borrow(session); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        const auto &plan = *retainedPlan_;
        const auto type = plan.Profile() == XRFeatureProfile::FoveatedInsetProjection
                              ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET
                              : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        return Outcome::Success({type, plan.Limits().maximumViews});
    }

    /** @copydoc OpenXRNativeSession::Borrow */
    Result<NativeSessionBorrow> OpenXRNativeSession::Borrow(const XRSessionId &session) const {
        const auto current = Validate(session);
        return current.HasError()
                   ? Result<NativeSessionBorrow>::Failure(current.ErrorValue())
                   : Result<NativeSessionBorrow>::Success({instance_, system_, session_, retainedPlan_->Limits().maximumActions,
                                                           retainedPlan_->Limits().maximumSpaces, nativeSessionIncarnation_});
    }
}  // namespace Horo::XR::OpenXRInternal
