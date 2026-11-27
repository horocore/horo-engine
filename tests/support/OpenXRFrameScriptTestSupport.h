#pragma once
/** @file OpenXRFrameScriptTestSupport.h
 * @brief Existing official native dispatch script shared by lifecycle and application tests.
 */
#include "OpenXRFrameCompositor.h"
#include "support/OpenXRNativeTestSupport.h"

#include <algorithm>
#include <bit>
#include <functional>

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
        std::function<void()> onEnd;
        std::uint32_t waits{};
        std::uint32_t begins{};
        std::uint32_t ends{};
        std::uint32_t layers{};
        std::uint32_t views{};
        std::uint32_t expectedViews{2};
        XrPassthroughLayerFB passthroughLayer{XR_NULL_HANDLE};
        XrCompositionLayerFlags projectionFlags{};
        std::array<XrCompositionLayerProjectionView, XRHardLimits::MaximumViews> encoded{};
        std::array<XrCompositionLayerDepthInfoKHR, XRHardLimits::MaximumViews> depths{};
        std::array<XrFrameSynthesisInfoEXT, 2> ext{};
        std::array<XrCompositionLayerSpaceWarpInfoFB, 2> fb{};

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
            active->passthroughLayer = XR_NULL_HANDLE;
            active->projectionFlags = 0;
            if (info->layerCount == 2) {
                const auto *underlay = reinterpret_cast<const XrCompositionLayerPassthroughFB *>(info->layers[0]);
                REQUIRE(underlay->type == XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB);
                CHECK(underlay->space == XR_NULL_HANDLE);
                CHECK(underlay->flags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
                active->passthroughLayer = underlay->layerHandle;
            }
            if (info->layerCount != 0) {
                const auto *layer = reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[info->layerCount - 1]);
                active->projectionFlags = layer->layerFlags;
                CHECK(layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
                active->views = layer->viewCount;
                REQUIRE(layer->viewCount == active->expectedViews);
                REQUIRE(layer->viewCount <= active->encoded.size());
                active->depths = {};
                active->ext = {};
                active->fb = {};
                for (std::size_t index = 0; index < layer->viewCount; ++index) {
                    active->encoded[index] = layer->views[index];
                    active->CaptureViewExtension(index, layer->views[index].next);
                }
            }
            if (active->onEnd)
                active->onEnd();
            return active->endResult;
        }

        void CaptureViewExtension(std::size_t index, const void *next) {
            if (!next)
                return;
            switch (static_cast<const XrBaseInStructure *>(next)->type) {
                case XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR:
                    depths[index] = *static_cast<const XrCompositionLayerDepthInfoKHR *>(next);
                    break;
                case XR_TYPE_FRAME_SYNTHESIS_INFO_EXT:
                    REQUIRE(index < ext.size());
                    ext[index] = *static_cast<const XrFrameSynthesisInfoEXT *>(next);
                    break;
                case XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB:
                    REQUIRE(index < fb.size());
                    fb[index] = *static_cast<const XrCompositionLayerSpaceWarpInfoFB *>(next);
                    break;
                default:
                    FAIL("Unexpected projection view extension");
            }
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
