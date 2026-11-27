#include "host/OpenXRAndroidDiscovery.h"

#if defined(__ANDROID__)
#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <openxr/openxr_platform.h>
#include <span>
#include <string_view>
#include <utility>

namespace Horo::XR::OpenXRInternal {
    static_assert(AndroidOpenXRInstanceExtension == XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);

    namespace {
        /** @brief Resolves only official dispatch from the exact loader or its retained instance. */
        template <typename Function>
        XrResult Resolve(PFN_xrGetInstanceProcAddr dispatch, XrInstance instance, const char *name, Function &function) noexcept {
            if (!dispatch)
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            PFN_xrVoidFunction raw{};
            const auto result = dispatch(instance, name, &raw);
            if (result != XR_SUCCESS)
                return result;
            function = std::bit_cast<Function>(raw);
            return function ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        /** @brief Checks that the private host token owns every JNI input needed by native discovery. */
        bool HasApplicationLease(const NativeOpenXRAndroidApplication &application) noexcept {
            return application.vm && application.applicationContext && application.activity && application.lifetime;
        }

        /** @brief Requires bounded, versioned availability of the Android create-instance wire contract. */
        XrResult AdmitAndroidInstanceExtension(PFN_xrGetInstanceProcAddr dispatch) noexcept {
            PFN_xrEnumerateInstanceExtensionProperties enumerate{};
            auto result = Resolve(dispatch, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", enumerate);
            if (result != XR_SUCCESS)
                return result;
            constexpr std::size_t MaximumExtensions = 128;
            std::uint32_t count{};
            result = enumerate(nullptr, 0, &count, nullptr);
            if (result != XR_SUCCESS)
                return result;
            if (!count)
                return XR_ERROR_EXTENSION_NOT_PRESENT;
            if (count > MaximumExtensions)
                return XR_ERROR_SIZE_INSUFFICIENT;
            const auto expected = count;
            std::array<XrExtensionProperties, MaximumExtensions> properties{};
            for (auto &extension : properties)
                extension.type = XR_TYPE_EXTENSION_PROPERTIES;
            result = enumerate(nullptr, static_cast<std::uint32_t>(properties.size()), &count, properties.data());
            if (result != XR_SUCCESS)
                return result;
            if (count != expected)
                return XR_ERROR_RUNTIME_FAILURE;
            const auto matches = std::ranges::count_if(std::span{properties}.first(count), [](const XrExtensionProperties &entry) {
                constexpr std::string_view Name{XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
                return entry.type == XR_TYPE_EXTENSION_PROPERTIES && entry.extensionVersion > 0 &&
                       std::string_view{entry.extensionName, Name.size()} == Name && entry.extensionName[Name.size()] == '\0';
            });
            if (matches > 1)
                return XR_ERROR_RUNTIME_FAILURE;
            return matches == 1 ? XR_SUCCESS : XR_ERROR_EXTENSION_NOT_PRESENT;
        }

        /** @brief Reads an HMD, numeric runtime version and optional recommended views from the retained cold instance. */
        AndroidOpenXRProbeResult DiscoverSystem(PFN_xrGetInstanceProcAddr dispatch, XrInstance instance) noexcept {
            using enum AndroidOpenXRProbeStage;
            PFN_xrGetSystem getSystem{};
            auto result = Resolve(dispatch, instance, "xrGetSystem", getSystem);
            if (result != XR_SUCCESS)
                return {SystemDiscovery, result};
            const XrSystemGetInfo request{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
            XrSystemId system{XR_NULL_SYSTEM_ID};
            result = getSystem(instance, &request, &system);
            if (result != XR_SUCCESS || system == XR_NULL_SYSTEM_ID)
                return {SystemDiscovery, result == XR_SUCCESS ? XR_ERROR_RUNTIME_FAILURE : result};

            PFN_xrGetInstanceProperties getProperties{};
            result = Resolve(dispatch, instance, "xrGetInstanceProperties", getProperties);
            if (result != XR_SUCCESS)
                return {RuntimeProperties, result};
            XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
            result = getProperties(instance, &properties);
            if (result != XR_SUCCESS)
                return {RuntimeProperties, result};
            if (properties.type != XR_TYPE_INSTANCE_PROPERTIES || properties.next || !XR_VERSION_MAJOR(properties.runtimeVersion))
                return {RuntimeProperties, XR_ERROR_RUNTIME_FAILURE};
            PFN_xrEnumerateViewConfigurationViews enumerate{};
            const auto resolved = Resolve(dispatch, instance, "xrEnumerateViewConfigurationViews", enumerate);
            return {Discovered, XR_SUCCESS, properties.runtimeVersion,
                    resolved == XR_SUCCESS ? ReadPrimaryStereoExtents(enumerate, instance, system) : std::nullopt};
        }
    }  // namespace

    /** @copydoc InitializeAndroidOpenXRLoader */
    XrResult InitializeAndroidOpenXRLoader(PFN_xrGetInstanceProcAddr dispatch, const NativeOpenXRAndroidApplication &application) noexcept {
        if (!HasApplicationLease(application))
            return XR_ERROR_VALIDATION_FAILURE;
        PFN_xrInitializeLoaderKHR initialize{};
        const auto resolved = Resolve(dispatch, XR_NULL_HANDLE, "xrInitializeLoaderKHR", initialize);
        if (resolved != XR_SUCCESS)
            return resolved;
        const XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, nullptr, application.vm,
                                              application.applicationContext};
        return initialize(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&info));
    }

    /** @copydoc CreateAndroidOpenXRInstance */
    XrResult CreateAndroidOpenXRInstance(PFN_xrCreateInstance create, const XrInstanceCreateInfo &info,
                                         const NativeOpenXRAndroidApplication &application, XrInstance &instance) noexcept {
        if (!create || !HasApplicationLease(application))
            return XR_ERROR_VALIDATION_FAILURE;
        const XrInstanceCreateInfoAndroidKHR activity{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, info.next, application.vm,
                                                      application.activity};
        auto request = info;
        request.next = &activity;
        return create(&request, &instance);
    }

    /** @copydoc AndroidOpenXRColdDiscovery::AndroidOpenXRColdDiscovery */
    AndroidOpenXRColdDiscovery::AndroidOpenXRColdDiscovery(std::shared_ptr<const Platform::DynamicLibrary> loader,
                                                           NativeOpenXRAndroidApplication application) noexcept
        : application_(std::move(application)), loader_(std::move(loader)) {}

    /** @copydoc AndroidOpenXRColdDiscovery::~AndroidOpenXRColdDiscovery */
    AndroidOpenXRColdDiscovery::~AndroidOpenXRColdDiscovery() {
        if (Retire() != XR_SUCCESS)
            std::terminate();
    }

    /** @copydoc AndroidOpenXRColdDiscovery::Probe */
    AndroidOpenXRProbeResult AndroidOpenXRColdDiscovery::Probe(XrVersion apiVersion) noexcept {
        using enum AndroidOpenXRProbeStage;
        if (controlLane_ != std::this_thread::get_id() || instance_ != XR_NULL_HANDLE)
            return {InstanceRetirement, XR_ERROR_CALL_ORDER_INVALID};
        if (XR_VERSION_MAJOR(apiVersion) != 1 || apiVersion > XR_CURRENT_API_VERSION)
            return {InstanceCreation, XR_ERROR_API_VERSION_UNSUPPORTED};
        const auto observed = Discover(apiVersion);
        const auto retired = Retire();
        return retired == XR_SUCCESS ? observed : AndroidOpenXRProbeResult{InstanceRetirement, retired};
    }

    /** @copydoc AndroidOpenXRColdDiscovery::Retire */
    XrResult AndroidOpenXRColdDiscovery::Retire() noexcept {
        if (controlLane_ != std::this_thread::get_id())
            return XR_ERROR_CALL_ORDER_INVALID;
        if (instance_ == XR_NULL_HANDLE)
            return XR_SUCCESS;
        const auto result = destroy_(instance_);
        if (result == XR_SUCCESS)
            instance_ = XR_NULL_HANDLE;
        return result;
    }

    /** @copydoc AndroidOpenXRColdDiscovery::Discover */
    AndroidOpenXRProbeResult AndroidOpenXRColdDiscovery::Discover(XrVersion apiVersion) noexcept {
        using enum AndroidOpenXRProbeStage;
        if (!loader_)
            return {LoaderDispatch, XR_ERROR_INITIALIZATION_FAILED};
        const auto dispatch = std::bit_cast<PFN_xrGetInstanceProcAddr>(loader_->GetSymbol("xrGetInstanceProcAddr"));
        destroy_ = std::bit_cast<PFN_xrDestroyInstance>(loader_->GetSymbol("xrDestroyInstance"));
        if (!dispatch || !destroy_)
            return {LoaderDispatch, XR_ERROR_FUNCTION_UNSUPPORTED};
        auto result = InitializeAndroidOpenXRLoader(dispatch, application_);
        if (result != XR_SUCCESS)
            return {LoaderInitialization, result};
        result = AdmitAndroidInstanceExtension(dispatch);
        if (result != XR_SUCCESS)
            return {ExtensionAdmission, result};
        PFN_xrCreateInstance create{};
        result = Resolve(dispatch, XR_NULL_HANDLE, "xrCreateInstance", create);
        if (result != XR_SUCCESS)
            return {InstanceCreation, result};

        const char *extension = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
        XrInstanceCreateInfo request{XR_TYPE_INSTANCE_CREATE_INFO};
        constexpr char ApplicationName[] = "Horo OpenXR Discovery";
        std::ranges::copy(ApplicationName, request.applicationInfo.applicationName);
        request.applicationInfo.apiVersion = apiVersion;
        request.enabledExtensionCount = 1;
        request.enabledExtensionNames = &extension;
        result = CreateAndroidOpenXRInstance(create, request, application_, instance_);
        if (result != XR_SUCCESS || instance_ == XR_NULL_HANDLE)
            return {InstanceCreation, result == XR_SUCCESS ? XR_ERROR_RUNTIME_FAILURE : result};
        return DiscoverSystem(dispatch, instance_);
    }
}  // namespace Horo::XR::OpenXRInternal
#endif
