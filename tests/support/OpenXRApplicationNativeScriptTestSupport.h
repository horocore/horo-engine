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
    /** @brief Fills the native pair from fixed scripted anatomical joint locations. */
    struct ApplicationHandScript final {
        static inline thread_local ApplicationHandScript *active{};
        std::array<bool, 2> live{};
        std::size_t locates{};
        std::size_t locateLookups{};
        bool failLocateLookup{};
        bool failRight{};
        bool malformed{};
        bool leftActive{true};
        XrResult rightLocateResult{XR_SUCCESS};

        static XRAPI_ATTR XrResult XRAPI_CALL Create(XrSession session, const XrHandTrackerCreateInfoEXT *info, XrHandTrackerEXT *tracker) {
            REQUIRE(session != XR_NULL_HANDLE);
            REQUIRE(info->handJointSet == XR_HAND_JOINT_SET_DEFAULT_EXT);
            const std::size_t side = info->hand == XR_HAND_LEFT_EXT ? 0 : 1;
            if (side == 1 && active->failRight)
                return XR_ERROR_PERMISSION_INSUFFICIENT;
            REQUIRE_FALSE(active->live[side]);
            active->live[side] = true;
            *tracker = OpenXR::NativeHandle<XrHandTrackerEXT>(side + 1);
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Destroy(XrHandTrackerEXT tracker) {
            const std::size_t side = tracker == OpenXR::NativeHandle<XrHandTrackerEXT>(1) ? 0 : 1;
            REQUIRE(active->live[side]);
            active->live[side] = false;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Locate(XrHandTrackerEXT tracker, const XrHandJointsLocateInfoEXT *info,
                                                     XrHandJointLocationsEXT *locations) {
            ++active->locates;
            REQUIRE(info->baseSpace != XR_NULL_HANDLE);
            REQUIRE(info->time > 0);
            REQUIRE(locations->jointCount == XR_HAND_JOINT_COUNT_EXT);
            const bool left = tracker == OpenXR::NativeHandle<XrHandTrackerEXT>(1);
            if (!left && active->rightLocateResult != XR_SUCCESS)
                return active->rightLocateResult;
            if (!left && active->malformed) {
                ++locations->jointCount;
                return XR_SUCCESS;
            }
            if (!left || !active->leftActive)
                return XR_SUCCESS;
            locations->isActive = XR_TRUE;
            for (auto &joint : std::span{locations->jointLocations, locations->jointCount}) {
                joint.locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                      XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
                joint.pose.orientation.w = 1.0F;
                joint.radius = 0.01F;
            }
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance, const char *name, PFN_xrVoidFunction *function) {
            struct Entry {
                std::string_view name;
                PFN_xrVoidFunction function;
            };

            const std::array routes{Entry{"xrCreateHandTrackerEXT", std::bit_cast<PFN_xrVoidFunction>(&Create)},
                                    Entry{"xrDestroyHandTrackerEXT", std::bit_cast<PFN_xrVoidFunction>(&Destroy)},
                                    Entry{"xrLocateHandJointsEXT", std::bit_cast<PFN_xrVoidFunction>(&Locate)}};
            const auto found = std::ranges::find(routes, std::string_view{name}, &Entry::name);
            if (found == routes.end())
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            if (found->name == "xrLocateHandJointsEXT") {
                ++active->locateLookups;
                if (active->failLocateLookup)
                    return XR_ERROR_FUNCTION_UNSUPPORTED;
            }
            *function = found->function;
            return XR_SUCCESS;
        }
    };

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
        ApplicationHandScript hands;
        OpenXR::FrameScript frames;
        OpenXR::SwapchainScript swapchain;

        ApplicationNativeScript() {
            REQUIRE(active == nullptr);
            native.legacyVulkan = true;
            native.clockExtension = true;
            native.extensionCount = 2;
            events.active = &events;
            hands.active = &hands;
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
            CHECK_FALSE(hands.live[0]);
            CHECK_FALSE(hands.live[1]);
            hands.active = nullptr;
            events.active = nullptr;
            active = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance instance, const char *name, PFN_xrVoidFunction *output) {
            *output = nullptr;
            if (!active)
                return XR_ERROR_INITIALIZATION_FAILED;
            if (active->native.absent == name || (instance != XR_NULL_HANDLE && active->native.instanceDispatchAbsent == name))
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            const std::array<PFN_xrGetInstanceProcAddr, 10> resolvers{&OpenXR::GraphicsScript::GetProc,
                                                                      &OpenXR::CensusScript::GetProc,
                                                                      &OpenXR::EventScript::GetProc,
                                                                      &OpenXR::FrameScript::GetProc,
                                                                      &OpenXR::ClockScript::Resolve,
                                                                      &OpenXR::SwapchainScript::GetProc,
                                                                      &OpenXRAction::ActionScript::GetProc,
                                                                      &OpenXRAction::PresentationScript::GetProc,
                                                                      &OpenXR::Script::GetProc,
                                                                      &ApplicationHandScript::Resolve};
            for (const auto resolver : resolvers) {
                const auto result = resolver(instance, name, output);
                if (result == XR_SUCCESS && *output)
                    return result;
            }
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
    };
}  // namespace Horo::Tests::OpenXRApplication
