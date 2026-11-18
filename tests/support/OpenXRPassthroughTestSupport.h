#pragma once
/** @file OpenXRPassthroughTestSupport.h @brief Shared scripted reconstruction dispatch. */
#include "support/OpenXRCensusTestSupport.h"
#include "support/OpenXRFrameScriptTestSupport.h"

namespace Horo::Tests::OpenXR {
    /** @brief Scripted v3 native provider behind the real selected session and compositor. */
    struct PassthroughScript final {
        static inline PassthroughScript *active{};
        XrResult createResult{XR_SUCCESS};
        XrResult resumeResult{XR_SUCCESS};
        bool failLayerDestroy{};
        bool liveFeature{};
        bool liveLayer{};
        bool running{};
        std::uint32_t featuresCreated{};
        std::uint32_t pauses{};
        std::uint32_t layersDestroyed{};

        PassthroughScript() {
            active = this;
        }

        ~PassthroughScript() {
            active = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Create(XrSession session, const XrPassthroughCreateInfoFB *info, XrPassthroughFB *feature) {
            CHECK(session != XR_NULL_HANDLE);
            CHECK(info->type == XR_TYPE_PASSTHROUGH_CREATE_INFO_FB);
            CHECK(info->flags == 0);
            if (active->createResult != XR_SUCCESS)
                return active->createResult;
            CHECK_FALSE(active->liveFeature);
            ++active->featuresCreated;
            active->liveFeature = true;
            *feature = NativeHandle<XrPassthroughFB>(29);
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL CreateLayer(XrSession session, const XrPassthroughLayerCreateInfoFB *info,
                                                          XrPassthroughLayerFB *layer) {
            CHECK(session != XR_NULL_HANDLE);
            CHECK(info->passthrough == NativeHandle<XrPassthroughFB>(29));
            CHECK(info->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB);
            CHECK(info->flags == 0);
            CHECK(active->liveFeature);
            CHECK_FALSE(active->liveLayer);
            active->liveLayer = true;
            *layer = NativeHandle<XrPassthroughLayerFB>(30);
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Start(XrPassthroughFB feature) {
            CHECK(feature == NativeHandle<XrPassthroughFB>(29));
            CHECK(active->liveFeature);
            CHECK_FALSE(active->running);
            active->running = true;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Resume(XrPassthroughLayerFB layer) {
            CHECK(layer == NativeHandle<XrPassthroughLayerFB>(30));
            CHECK(active->running);
            CHECK(active->liveLayer);
            return active->resumeResult;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Pause(XrPassthroughFB feature) {
            CHECK(feature == NativeHandle<XrPassthroughFB>(29));
            CHECK(active->liveFeature);
            CHECK(active->running);
            active->running = false;
            ++active->pauses;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL DestroyLayer(XrPassthroughLayerFB layer) {
            CHECK(layer == NativeHandle<XrPassthroughLayerFB>(30));
            CHECK(active->liveLayer);
            CHECK_FALSE(active->running);
            if (active->failLayerDestroy)
                return XR_ERROR_RUNTIME_FAILURE;
            active->liveLayer = false;
            ++active->layersDestroyed;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Destroy(XrPassthroughFB feature) {
            CHECK(feature == NativeHandle<XrPassthroughFB>(29));
            CHECK(active->liveFeature);
            CHECK_FALSE(active->liveLayer);
            active->liveFeature = false;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
            const std::string_view symbol{name};
            const std::array entries{std::pair{"xrCreatePassthroughFB", std::bit_cast<PFN_xrVoidFunction>(&Create)},
                                     std::pair{"xrCreatePassthroughLayerFB", std::bit_cast<PFN_xrVoidFunction>(&CreateLayer)},
                                     std::pair{"xrPassthroughStartFB", std::bit_cast<PFN_xrVoidFunction>(&Start)},
                                     std::pair{"xrPassthroughLayerResumeFB", std::bit_cast<PFN_xrVoidFunction>(&Resume)},
                                     std::pair{"xrPassthroughPauseFB", std::bit_cast<PFN_xrVoidFunction>(&Pause)},
                                     std::pair{"xrDestroyPassthroughLayerFB", std::bit_cast<PFN_xrVoidFunction>(&DestroyLayer)},
                                     std::pair{"xrDestroyPassthroughFB", std::bit_cast<PFN_xrVoidFunction>(&Destroy)},
                                     std::pair{"xrGetSystemProperties", std::bit_cast<PFN_xrVoidFunction>(&CensusScript::System)}};
            for (const auto &[entry, pointer] : entries) {
                if (entry == symbol) {
                    *function = pointer;
                    return XR_SUCCESS;
                }
            }
            return FrameScript::GetProc(instance, name, function);
        }
    };
}  // namespace Horo::Tests::OpenXR
