#pragma once
/** @file OpenXRPresentationTestSupport.h
 * @brief Canonical scripted presentation ABI shared by pose and installed-application fixtures.
 */
#include "support/OpenXRActionBindingTestSupport.h"

namespace Horo::Tests::OpenXRAction {
    /** @brief Official native presentation dispatch with independently controlled complete count/validity evidence. */
    struct PresentationScript final {
        static inline thread_local PresentationScript *active{};
        XrViewStateFlags flags{XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT |
                               XR_VIEW_STATE_ORIENTATION_TRACKED_BIT};
        std::uint32_t count{2};
        XrTime time{};
        XrSpace reference{};
        bool malformedFov{};
        bool staleAfterLocate{};

        PresentationScript() {
            REQUIRE(active == nullptr);
            active = this;
        }

        ~PresentationScript() {
            active = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Locate(XrSession session, const XrViewLocateInfo *info, XrViewState *state,
                                                     std::uint32_t capacity, std::uint32_t *count, XrView *views) {
            REQUIRE(session != XR_NULL_HANDLE);
            REQUIRE(info->type == XR_TYPE_VIEW_LOCATE_INFO);
            REQUIRE(info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
            REQUIRE(info->space != XR_NULL_HANDLE);
            REQUIRE(capacity == 2);
            active->time = info->displayTime;
            active->reference = info->space;
            state->viewStateFlags = active->flags;
            *count = active->count;
            for (std::uint32_t index = 0; index < std::min(capacity, active->count); ++index) {
                REQUIRE(views[index].type == XR_TYPE_VIEW);
                views[index].pose = {{0, 0, 0, 1}, {index == 0 ? -0.032F : 0.032F, 0, 0}};
                views[index].fov = {-0.8F, 0.9F, 0.7F, -0.6F};
                if (active->malformedFov)
                    views[index].fov.angleLeft = std::numeric_limits<float>::quiet_NaN();
            }
            if (active->staleAfterLocate)
                Script::active->stale = true;
            return Script::Record("locate-views");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Enumerate(XrInstance instance, XrSystemId system, XrViewConfigurationType type,
                                                        std::uint32_t capacity, std::uint32_t *count, XrViewConfigurationView *views) {
            REQUIRE(instance != XR_NULL_HANDLE);
            REQUIRE(system != XR_NULL_SYSTEM_ID);
            REQUIRE(type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
            *count = 2;
            if (capacity) {
                REQUIRE(capacity == 2);
                for (std::uint32_t index = 0; index < capacity; ++index) {
                    REQUIRE(views[index].type == XR_TYPE_VIEW_CONFIGURATION_VIEW);
                    views[index].recommendedImageRectWidth = views[index].maxImageRectWidth = 640;
                    views[index].recommendedImageRectHeight = views[index].maxImageRectHeight = 480;
                }
            }
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance instance, const char *name, PFN_xrVoidFunction *output) {
            if (std::string_view{name} == "xrEnumerateViewConfigurationViews") {
                *output = std::bit_cast<PFN_xrVoidFunction>(&Enumerate);
                return XR_SUCCESS;
            }
            if (std::string_view{name} == "xrLocateViews" && Script::active->absent != name) {
                *output = std::bit_cast<PFN_xrVoidFunction>(&Locate);
                return XR_SUCCESS;
            }
            return ActionScript::GetProc(instance, name, output);
        }
    };
}  // namespace Horo::Tests::OpenXRAction
