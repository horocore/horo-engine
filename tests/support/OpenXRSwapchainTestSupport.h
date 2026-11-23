#pragma once

#include "OpenXRNativeTestSupport.h"
#include "OpenXRSwapchain.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <optional>

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
        std::optional<std::int64_t> rejectCreateFormat;
        XrResult failure{XR_ERROR_RUNTIME_FAILURE};
        std::uint32_t formatCount{1};
        std::int64_t format{37};
        std::uint32_t imageCount{3};
        std::uint32_t imageIndex{};
        std::uint32_t creates{};
        std::uint32_t releases{};
        std::function<void()> onRelease;
        std::uint32_t destroys{};
        bool timeout{};
        XrSwapchain timeoutSwapchain{XR_NULL_HANDLE};
        XrSwapchainCreateInfo created{};
        std::array<XrSwapchainSubImage, 2> fixedProjection{};
        XrSession fixedFoveationSession{XR_NULL_HANDLE};
        std::vector<XrFoveationModeHTC> foveationModes;
        bool rejectGazeProfile{};
        std::array<XrFoveationConfigurationHTC, 2> gazeConfigurations{};

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
            if (active->rejectCreateFormat == info->format)
                return active->failure;
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

        static XRAPI_ATTR XrResult XRAPI_CALL Images(XrSwapchain swapchain, std::uint32_t capacity, std::uint32_t *count,
                                                     XrSwapchainImageBaseHeader *images) {
            *count = active->imageCount;
#if defined(XR_USE_GRAPHICS_API_VULKAN)
            if (capacity >= *count && images != nullptr) {
                auto *typed = reinterpret_cast<XrSwapchainImageVulkanKHR *>(images);
                // Each scripted swapchain owns a distinct bounded image range.
                const auto base = 900 + reinterpret_cast<std::uintptr_t>(swapchain) * 64;
                for (std::uint32_t index = 0; index < *count; ++index)
                    typed[index].image = NativeHandle<VkImage>(base + index);
            }
#else
            (void)swapchain;
            (void)capacity;
            (void)images;
#endif
            return Record("enumerate");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Acquire(XrSwapchain, const XrSwapchainImageAcquireInfo *, std::uint32_t *index) {
            *index = active->imageIndex;
            return Record("acquire");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Wait(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *info) {
            CHECK(info->timeout >= 0);
            CHECK(info->timeout <= 100'000'000);
            const auto result = Record("wait");
            return XR_SUCCEEDED(result) && (active->timeout || active->timeoutSwapchain == swapchain) ? XR_TIMEOUT_EXPIRED : result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Release(XrSwapchain, const XrSwapchainImageReleaseInfo *) {
            const auto result = Record("release");
            if (XR_SUCCEEDED(result)) {
                ++active->releases;
                if (active->onRelease)
                    active->onRelease();
            }
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL ApplyFixedFoveation(XrSession session, const XrFoveationApplyInfoHTC *info) {
            REQUIRE(info);
            CHECK(info->type == XR_TYPE_FOVEATION_APPLY_INFO_HTC);
            active->foveationModes.push_back(info->mode);
            if (info->mode == XR_FOVEATION_MODE_CUSTOM_HTC) {
                const auto *custom = static_cast<const XrFoveationCustomModeInfoHTC *>(info->next);
                REQUIRE(custom);
                CHECK(custom->type == XR_TYPE_FOVEATION_CUSTOM_MODE_INFO_HTC);
                REQUIRE(custom->configCount == active->gazeConfigurations.size());
                REQUIRE(custom->configs);
                std::copy_n(custom->configs, custom->configCount, active->gazeConfigurations.begin());
            } else {
                CHECK(info->next == nullptr);
                CHECK(info->mode == XR_FOVEATION_MODE_FIXED_HTC);
            }
            REQUIRE(info->subImageCount == active->fixedProjection.size());
            REQUIRE(info->subImages);
            std::copy_n(info->subImages, info->subImageCount, active->fixedProjection.begin());
            active->fixedFoveationSession = session;
            if (info->mode == XR_FOVEATION_MODE_CUSTOM_HTC && active->rejectGazeProfile)
                return XR_ERROR_FEATURE_UNSUPPORTED;
            return Record("foveation");
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
                                     Entry{"xrReleaseSwapchainImage", std::bit_cast<PFN_xrVoidFunction>(&Release)},
                                     Entry{"xrApplyFoveationHTC", std::bit_cast<PFN_xrVoidFunction>(&ApplyFixedFoveation)}};
            const auto entry = std::ranges::find(entries, std::string_view{name}, &Entry::name);
            if (entry == entries.end())
                return Script::GetProc(instance, name, function);
            *function = entry->function;
            return XR_SUCCESS;
        }
    };

}  // namespace Horo::Tests::OpenXR
