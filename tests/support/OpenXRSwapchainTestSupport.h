#pragma once

#include "OpenXRNativeTestSupport.h"
#include "OpenXRSwapchain.h"

#include <algorithm>
#include <bit>

#if defined(XR_USE_GRAPHICS_API_VULKAN)
#include <vulkan/vulkan.h>

// OpenXR platform declarations require the Vulkan types above.
#include <openxr/openxr_platform.h>
#endif

namespace Horo::Tests::OpenXR {
    /** @brief Official swapchain dispatch script; production owner remains the code under test. */
    struct SwapchainScript final {
        static inline SwapchainScript *active{};
        std::vector<std::string_view> calls;
        std::string_view fail;
        XrResult failure{XR_ERROR_RUNTIME_FAILURE};
        std::uint32_t formatCount{1};
        std::int64_t format{37};
        std::uint32_t imageCount{3};
        std::uint32_t imageIndex{};
        std::uint32_t creates{};
        std::uint32_t releases{};
        std::uint32_t destroys{};
        bool timeout{};
        XrSwapchainCreateInfo created{};

        SwapchainScript() {
            active = this;
        }

        ~SwapchainScript() {
            active = nullptr;
        }

        static XrResult Record(std::string_view operation) {
            active->calls.push_back(operation);
            return active->fail == operation ? active->failure : XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Formats(XrSession, std::uint32_t capacity, std::uint32_t *count, std::int64_t *formats) {
            *count = active->formatCount;
            if (capacity && *count <= capacity)
                std::fill_n(formats, *count, active->format);
            return Record("formats");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Create(XrSession, const XrSwapchainCreateInfo *info, XrSwapchain *swapchain) {
            const auto result = Record("create");
            if (XR_SUCCEEDED(result)) {
                active->created = *info;
                *swapchain = NativeHandle<XrSwapchain>(++active->creates);
            }
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Destroy(XrSwapchain) {
            const auto result = Record("destroy");
            if (XR_SUCCEEDED(result))
                ++active->destroys;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Images(XrSwapchain, std::uint32_t capacity, std::uint32_t *count,
                                                     XrSwapchainImageBaseHeader *images) {
            *count = active->imageCount;
#if defined(XR_USE_GRAPHICS_API_VULKAN)
            if (capacity >= *count && images != nullptr) {
                auto *typed = reinterpret_cast<XrSwapchainImageVulkanKHR *>(images);
                for (std::uint32_t index = 0; index < *count; ++index)
                    typed[index].image = NativeHandle<VkImage>(900 + index);
            }
#else
            (void)capacity;
            (void)images;
#endif
            return Record("enumerate");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Acquire(XrSwapchain, const XrSwapchainImageAcquireInfo *, std::uint32_t *index) {
            *index = active->imageIndex;
            return Record("acquire");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Wait(XrSwapchain, const XrSwapchainImageWaitInfo *info) {
            CHECK(info->timeout >= 0);
            CHECK(info->timeout <= 100'000'000);
            const auto result = Record("wait");
            return XR_SUCCEEDED(result) && active->timeout ? XR_TIMEOUT_EXPIRED : result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Release(XrSwapchain, const XrSwapchainImageReleaseInfo *) {
            const auto result = Record("release");
            if (XR_SUCCEEDED(result))
                ++active->releases;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
            if (Script::active->absent == name)
                return XR_ERROR_FUNCTION_UNSUPPORTED;

            struct Entry {
                std::string_view name;
                PFN_xrVoidFunction function;
            };

            const std::array entries{Entry{"xrEnumerateSwapchainFormats", std::bit_cast<PFN_xrVoidFunction>(&Formats)},
                                     Entry{"xrCreateSwapchain", std::bit_cast<PFN_xrVoidFunction>(&Create)},
                                     Entry{"xrDestroySwapchain", std::bit_cast<PFN_xrVoidFunction>(&Destroy)},
                                     Entry{"xrEnumerateSwapchainImages", std::bit_cast<PFN_xrVoidFunction>(&Images)},
                                     Entry{"xrAcquireSwapchainImage", std::bit_cast<PFN_xrVoidFunction>(&Acquire)},
                                     Entry{"xrWaitSwapchainImage", std::bit_cast<PFN_xrVoidFunction>(&Wait)},
                                     Entry{"xrReleaseSwapchainImage", std::bit_cast<PFN_xrVoidFunction>(&Release)}};
            const auto entry = std::ranges::find(entries, std::string_view{name}, &Entry::name);
            if (entry == entries.end())
                return Script::GetProc(instance, name, function);
            *function = entry->function;
            return XR_SUCCESS;
        }
    };

}  // namespace Horo::Tests::OpenXR
