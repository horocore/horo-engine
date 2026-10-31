#pragma once
/** @file OpenXRSelectedVulkanTestSupport.h @brief Existing official graphics-requirements script; no native device qualification. */
#include "OpenXRNativeSession.h"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vulkan/vulkan.h>

// OpenXR extension declarations require the official Vulkan types above.
#include <openxr/openxr_platform.h>

namespace Horo::Tests::OpenXR {
    using namespace Horo::XR::OpenXRInternal;

    struct GraphicsScript {
        inline static GraphicsScript *active{};
        std::string instanceExtensions{"VK_KHR_get_physical_device_properties2"};
        std::string deviceExtensions{"VK_KHR_external_memory"};
        XrResult failure{XR_SUCCESS};
        VkInstance queriedInstance{};
        VkPhysicalDevice selected{std::bit_cast<VkPhysicalDevice>(std::uintptr_t{7})};

        GraphicsScript() {
            active = this;
        }

        ~GraphicsScript() {
            active = nullptr;
        }

        static XrResult XRAPI_PTR Requirements(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR *out) {
            out->minApiVersionSupported = XR_MAKE_VERSION(1, 2, 0);
            out->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
            return active->failure;
        }

        static XrResult Names(const std::string &names, std::uint32_t capacity, std::uint32_t *count, char *out) {
            *count = static_cast<std::uint32_t>(names.size() + 1);
            if (capacity) {
                if (capacity < *count)
                    return XR_ERROR_SIZE_INSUFFICIENT;
                std::copy(names.begin(), names.end(), out);
                out[names.size()] = '\0';
            }
            return XR_SUCCESS;
        }

        static XrResult XRAPI_PTR InstanceNames(XrInstance, XrSystemId, std::uint32_t c, std::uint32_t *n, char *p) {
            return Names(active->instanceExtensions, c, n, p);
        }

        static XrResult XRAPI_PTR DeviceNames(XrInstance, XrSystemId, std::uint32_t c, std::uint32_t *n, char *p) {
            return Names(active->deviceExtensions, c, n, p);
        }

        static XrResult XRAPI_PTR Device(XrInstance, XrSystemId, VkInstance instance, VkPhysicalDevice *out) {
            active->queriedInstance = instance;
            *out = active->selected;
            return active->failure;
        }

        static XrResult XRAPI_PTR GetProc(XrInstance, const char *name, PFN_xrVoidFunction *out) {
            *out = nullptr;
            if (std::strcmp(name, "xrGetVulkanGraphicsRequirementsKHR") == 0)
                *out = std::bit_cast<PFN_xrVoidFunction>(&Requirements);
            if (std::strcmp(name, "xrGetVulkanInstanceExtensionsKHR") == 0)
                *out = std::bit_cast<PFN_xrVoidFunction>(&InstanceNames);
            if (std::strcmp(name, "xrGetVulkanDeviceExtensionsKHR") == 0)
                *out = std::bit_cast<PFN_xrVoidFunction>(&DeviceNames);
            if (std::strcmp(name, "xrGetVulkanGraphicsDeviceKHR") == 0)
                *out = std::bit_cast<PFN_xrVoidFunction>(&Device);
            return *out ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        NativeDiscoveryBorrow Borrow() {
            static constexpr std::array<const char *, 1> extensions{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
            return {std::bit_cast<XrInstance>(std::uintptr_t{1}), 2, &GetProc, extensions, {1, 0, 0}};
        }
    };
}  // namespace Horo::Tests::OpenXR
