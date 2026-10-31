#pragma once
/** @file OpenXRApplicationNativeScriptTestSupport.h
 * @brief Portable composition of existing OpenXR test-script dispatch, without editor/Vulkan renderer ports.
 */
#include "OpenXRSelectedVulkanTestSupport.h"
#include "support/OpenXRActionBindingTestSupport.h"
#include "support/OpenXRCensusTestSupport.h"
#include "support/OpenXRClockTestSupport.h"
#include "support/OpenXREventScriptTestSupport.h"
#include "support/OpenXRFrameScriptTestSupport.h"
#include "support/OpenXRPresentationTestSupport.h"
#include "support/OpenXRSwapchainTestSupport.h"

namespace Horo::Tests::OpenXRApplication {
    /** @brief Keeps the exact existing script authorities alive until all loaded application children unload. */
    struct ApplicationNativeScript final {
        static inline thread_local ApplicationNativeScript *active{};
        OpenXR::Script native;
        OpenXR::CensusScript census;
        OpenXR::GraphicsScript graphics;
        OpenXRAction::ActionScript actions;
        OpenXRAction::PresentationScript presentation;
        OpenXR::ClockScript clock;
        OpenXR::EventScript events;
        OpenXR::FrameScript frames;
        OpenXR::SwapchainScript swapchain;

        ApplicationNativeScript() {
            REQUIRE(active == nullptr);
            native.legacyVulkan = true;
            native.clockExtension = true;
            native.extensionCount = 2;
            events.active = &events;
            active = this;
        }

        ApplicationNativeScript(const ApplicationNativeScript &) = delete;
        ApplicationNativeScript &operator=(const ApplicationNativeScript &) = delete;

        ~ApplicationNativeScript() {
            CHECK(native.liveInstances == 0);
            CHECK(native.liveSessions == 0);
            CHECK(actions.liveSets == 0);
            CHECK(actions.liveActions == 0);
            CHECK(actions.tracking.spaces == 0);
            events.active = nullptr;
            active = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance instance, const char *name, PFN_xrVoidFunction *output) {
            *output = nullptr;
            if (!active)
                return XR_ERROR_INITIALIZATION_FAILED;
            if (active->native.absent == name || (instance != XR_NULL_HANDLE && active->native.instanceDispatchAbsent == name))
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            const std::array<PFN_xrGetInstanceProcAddr, 8> resolvers{&OpenXR::GraphicsScript::GetProc,
                                                                     &OpenXR::CensusScript::GetProc,
                                                                     &OpenXR::EventScript::GetProc,
                                                                     &OpenXR::FrameScript::GetProc,
                                                                     &OpenXR::ClockScript::Resolve,
                                                                     &OpenXR::SwapchainScript::GetProc,
                                                                     &OpenXRAction::ActionScript::GetProc,
                                                                     &OpenXRAction::PresentationScript::GetProc};
            for (const auto resolver : resolvers) {
                const auto result = resolver(instance, name, output);
                if (result == XR_SUCCESS && *output)
                    return result;
            }
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
    };
}  // namespace Horo::Tests::OpenXRApplication
