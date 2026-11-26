#pragma once

#include "OpenXRCensusTestSupport.h"

#include <bit>
#include <functional>
#include <limits>

namespace Horo::Tests::OpenXR {
    /** @brief Models only exact v1 dispatch and per-slot validity; native session ownership is real. */
    struct LightScript final {
        static inline LightScript *active{};
        static constexpr XrTime InitialTime = 100;
        XrTime sampleTime{InitialTime};
        XrResult createResult{XR_SUCCESS};
        XrResult sampleResult{XR_SUCCESS};
        std::uint32_t createCalls{};
        std::uint32_t sampleCalls{};
        bool ambientValid{true};
        bool directionalValid{true};
        bool malformedDirection{};
        bool sampleAtCutoff{};
        std::function<void()> onCreate;
        std::function<void()> onSample;
        XrSpace expectedSpace{NativeHandle<XrSpace>(15)};
        bool failRetirement{};
        bool live{};
        std::uint32_t retireCalls{};

        LightScript() {
            active = this;
        }

        ~LightScript() {
            active = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Create(XrSession session, XrLightEstimatorCreateInfoANDROID *info,
                                                     XrLightEstimatorANDROID *handle) {
            CHECK(session != XR_NULL_HANDLE);
            CHECK(info->type == XR_TYPE_LIGHT_ESTIMATOR_CREATE_INFO_ANDROID);
            CHECK(info->next == nullptr);
            ++active->createCalls;
            if (active->createResult == XR_SUCCESS) {
                CHECK_FALSE(active->live);
                active->live = true;
                *handle = NativeHandle<XrLightEstimatorANDROID>(73);
            }
            if (active->onCreate)
                active->onCreate();
            return active->createResult;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Destroy(XrLightEstimatorANDROID handle) {
            CHECK(handle == NativeHandle<XrLightEstimatorANDROID>(73));
            CHECK(active->live);
            ++active->retireCalls;
            if (active->failRetirement)
                return XR_ERROR_RUNTIME_FAILURE;
            active->live = false;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Sample(XrLightEstimatorANDROID handle, const XrLightEstimateGetInfoANDROID *info,
                                                     XrLightEstimateANDROID *estimate) {
            CHECK(active->live);
            CHECK(handle == NativeHandle<XrLightEstimatorANDROID>(73));
            CHECK(info->type == XR_TYPE_LIGHT_ESTIMATE_GET_INFO_ANDROID);
            CHECK(info->space != XR_NULL_HANDLE);
            if (active->expectedSpace != XR_NULL_HANDLE)
                CHECK(info->space == active->expectedSpace);
            CHECK(info->time > 0);
            ++active->sampleCalls;
            if (active->onSample)
                active->onSample();
            if (active->sampleResult != XR_SUCCESS)
                return active->sampleResult;
            estimate->state = XR_LIGHT_ESTIMATE_STATE_VALID_ANDROID;
            estimate->lastUpdatedTime = active->sampleAtCutoff ? info->time : active->sampleTime;
            auto &ambient = *static_cast<XrAmbientLightANDROID *>(estimate->next);
            CHECK(ambient.type == XR_TYPE_AMBIENT_LIGHT_ANDROID);
            const auto invalid = std::numeric_limits<float>::quiet_NaN();
            if (active->ambientValid) {
                ambient.state = XR_LIGHT_ESTIMATE_STATE_VALID_ANDROID;
                ambient.intensity = {1.0F, 3.0F, 2.0F};
            } else {
                ambient.intensity = {invalid, invalid, invalid};
            }
            auto &direction = *static_cast<XrDirectionalLightANDROID *>(ambient.next);
            CHECK(direction.type == XR_TYPE_DIRECTIONAL_LIGHT_ANDROID);
            if (active->directionalValid) {
                direction.state = XR_LIGHT_ESTIMATE_STATE_VALID_ANDROID;
                direction.intensity = {4.0F, 2.0F, 3.0F};
            }
            direction.direction = {0.0F, active->malformedDirection ? invalid : -2.0F, 0.0F};
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance, const char *name, PFN_xrVoidFunction *function) {
            const std::string_view symbol{name};
            if (symbol == "xrCreateLightEstimatorANDROID")
                *function = std::bit_cast<PFN_xrVoidFunction>(&Create);
            else if (symbol == "xrDestroyLightEstimatorANDROID")
                *function = std::bit_cast<PFN_xrVoidFunction>(&Destroy);
            else if (symbol == "xrGetLightEstimateANDROID")
                *function = std::bit_cast<PFN_xrVoidFunction>(&Sample);
            else
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            return XR_SUCCESS;
        }
    };

}  // namespace Horo::Tests::OpenXR
