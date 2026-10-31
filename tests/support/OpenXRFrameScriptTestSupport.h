#pragma once
/** @file OpenXRFrameScriptTestSupport.h
 * @brief Existing official native dispatch script shared by lifecycle and application tests.
 */
#include "OpenXRFrameCompositor.h"
#include "support/OpenXRNativeTestSupport.h"

#include <algorithm>
#include <bit>

namespace Horo::Tests::OpenXR {
    /** @brief Official native frame dispatch script, never a physical-device qualification. */
    struct FrameScript final {
        static inline FrameScript *active{};
        XrResult waitResult{XR_SUCCESS};
        XrResult beginResult{XR_SUCCESS};
        XrResult endResult{XR_SUCCESS};
        XrTime prediction{10'000'000};
        XrDuration period{11'111'111};
        bool render{true};
        bool missingEnd{};
        NativeFramePacingPolicy *mutatePacing{};
        std::uint32_t waits{};
        std::uint32_t begins{};
        std::uint32_t ends{};
        std::uint32_t layers{};
        std::uint32_t views{};
        std::array<XrCompositionLayerProjectionView, 2> encoded{};
        std::array<XrCompositionLayerDepthInfoKHR, 2> depths{};

        FrameScript() {
            active = this;
        }

        ~FrameScript() {
            active = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Wait(XrSession, const XrFrameWaitInfo *, XrFrameState *state) {
            ++active->waits;
            state->predictedDisplayTime = active->prediction;
            state->predictedDisplayPeriod = active->period;
            state->shouldRender = active->render ? XR_TRUE : XR_FALSE;
            return active->waitResult;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Begin(XrSession, const XrFrameBeginInfo *) {
            ++active->begins;
            return active->beginResult;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL End(XrSession, const XrFrameEndInfo *info) {
            ++active->ends;
            CHECK(info->displayTime == active->prediction);
            CHECK(info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
            active->layers = info->layerCount;
            if (info->layerCount == 1) {
                const auto *layer = reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[0]);
                CHECK(layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
                active->views = layer->viewCount;
                REQUIRE(layer->viewCount == 2);
                for (std::size_t index = 0; index < 2; ++index) {
                    active->encoded[index] = layer->views[index];
                    if (layer->views[index].next)
                        active->depths[index] = *static_cast<const XrCompositionLayerDepthInfoKHR *>(layer->views[index].next);
                }
            }
            return active->endResult;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance, const char *name, PFN_xrVoidFunction *function) {
            if (active->mutatePacing)
                *active->mutatePacing = {.gpuBudgetNanoseconds = 9'000'000,
                                         .compositorMarginNanoseconds = 9'000'000,
                                         .maximumFramesInFlight = 2};
            *function = nullptr;
            if (std::string_view{name} == "xrWaitFrame")
                *function = std::bit_cast<PFN_xrVoidFunction>(&Wait);
            if (std::string_view{name} == "xrBeginFrame")
                *function = std::bit_cast<PFN_xrVoidFunction>(&Begin);
            if (std::string_view{name} == "xrEndFrame" && !active->missingEnd)
                *function = std::bit_cast<PFN_xrVoidFunction>(&End);
            return *function ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
        }
    };

}  // namespace Horo::Tests::OpenXR
