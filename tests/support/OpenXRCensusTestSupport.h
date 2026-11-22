#pragma once
/** @file OpenXRCensusTestSupport.h @brief Existing official system/session census script shared by native and real-loader tests. */
#include "support/OpenXRNativeTestSupport.h"

#include <bit>

namespace Horo::Tests::OpenXR {
    struct CensusScript {
        static inline CensusScript *active{};
        XrVersion implementationVersion{XR_MAKE_VERSION(9, 8, 7)};
        std::uint32_t views{2};
        std::uint32_t quadViews{4};
        bool advertisesQuad{};
        bool invalidQuadView{};
        bool duplicateQuadBlend{};
        std::uint32_t spaces{2};
        std::uint32_t formats{2};
        std::span<const std::int64_t> formatPublication;
        bool duplicateSpaces{};
        bool invalidView{};
        bool handsSupported{};
        XrBool32 gazeSupport{XR_FALSE};
        XrPassthroughCapabilityFlagsFB passthroughCapabilities{};
        std::uint32_t maximumLayers{16};
        XrResult gazeResult{XR_SUCCESS};
        XrBool32 lightEstimationSupport{XR_FALSE};
        bool spatialPlanes{true};
        bool spatialBounds{true};
        bool spatialLabels{true};
        XrResult spatialComponentsResult{XR_SUCCESS};
        bool invalidReprojectionExtent{};
        std::uint32_t synthesisEyeWidthStep{160};
        std::uint32_t synthesisEyeQueries{};
        std::string_view fail;
        PFN_xrGetInstanceProcAddr extraProc{};
        std::vector<std::string_view> calls;

        CensusScript() {
            REQUIRE(active == nullptr);
            active = this;
        }

        ~CensusScript() {
            active = nullptr;
        }

        static XrResult Record(const std::string_view phase) {
            active->calls.push_back(phase);
            return active->fail == phase ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Instance(XrInstance, XrInstanceProperties *value) {
            CopyNativeLiteral(value->runtimeName, "Actual scripted runtime");
            value->runtimeVersion = active->implementationVersion;
            return Record("instance-properties");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL System(XrInstance, XrSystemId system, XrSystemProperties *value) {
            value->systemId = system;
            CopyNativeLiteral(value->systemName, "Observed native system");
            value->graphicsProperties = {2048, 2048, active->maximumLayers};
            value->trackingProperties = {XR_TRUE, XR_TRUE};
            if (value->next) {
                const auto *header = static_cast<const XrBaseOutStructure *>(value->next);
                if (header->type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT) {
                    auto &eyes = *static_cast<XrSystemEyeGazeInteractionPropertiesEXT *>(value->next);
                    eyes.supportsEyeGazeInteraction = active->gazeSupport;
                    active->calls.push_back("eye-gaze-properties");
                    return active->gazeResult;
                }
                if (header->type == XR_TYPE_SYSTEM_LIGHT_ESTIMATION_PROPERTIES_ANDROID) {
                    auto &light = *static_cast<XrSystemLightEstimationPropertiesANDROID *>(value->next);
                    light.supportsLightEstimation = active->lightEstimationSupport;
                    return Record("light-estimation-properties");
                }
                if (header->type == XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB) {
                    auto &passthrough = *static_cast<XrSystemPassthroughProperties2FB *>(value->next);
                    passthrough.capabilities = active->passthroughCapabilities;
                    return Record("passthrough-properties");
                }
                if (header->type == XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB) {
                    auto &warp = *static_cast<XrSystemSpaceWarpPropertiesFB *>(value->next);
                    warp.recommendedMotionVectorImageRectWidth = active->invalidReprojectionExtent ? 4096 : 640;
                    warp.recommendedMotionVectorImageRectHeight = 512;
                } else {
                    auto &hands = *static_cast<XrSystemHandTrackingPropertiesEXT *>(value->next);
                    REQUIRE(hands.type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT);
                    hands.supportsHandTracking = active->handsSupported ? XR_TRUE : XR_FALSE;
                }
            }
            return Record("system-properties");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL SpatialCapabilities(XrInstance, XrSystemId, std::uint32_t capacity, std::uint32_t *count,
                                                                  XrSpatialCapabilityEXT *values) {
            CHECK(capacity > 0);
            REQUIRE(values != nullptr);
            *count = active->spatialPlanes ? 1 : 0;
            if (*count)
                values[0] = XR_SPATIAL_CAPABILITY_PLANE_TRACKING_EXT;
            return Record("spatial-capabilities");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL SpatialComponents(XrInstance, XrSystemId, XrSpatialCapabilityEXT capability,
                                                                XrSpatialCapabilityComponentTypesEXT *values) {
            CHECK(capability == XR_SPATIAL_CAPABILITY_PLANE_TRACKING_EXT);
            REQUIRE(values->componentTypeCapacityInput >= 2);
            REQUIRE(values->componentTypes != nullptr);
            if (active->spatialComponentsResult != XR_SUCCESS)
                return active->spatialComponentsResult;
            values->componentTypeCountOutput = active->spatialLabels ? 2 : 1;
            values->componentTypes[0] =
                active->spatialBounds ? XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT : XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT;
            if (active->spatialLabels)
                values->componentTypes[1] = XR_SPATIAL_COMPONENT_TYPE_PLANE_SEMANTIC_LABEL_EXT;
            return Record("spatial-components");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Configurations(XrInstance, XrSystemId, const std::uint32_t capacity, std::uint32_t *count,
                                                             XrViewConfigurationType *values) {
            *count = active->advertisesQuad ? 2 : 1;
            if (capacity < *count)
                return capacity == 0 ? Record("configurations") : XR_ERROR_SIZE_INSUFFICIENT;
            values[0] = active->advertisesQuad ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET
                                               : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (active->advertisesQuad)
                values[1] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            return Record("configurations");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Views(XrInstance, XrSystemId, const XrViewConfigurationType type,
                                                    const std::uint32_t capacity, std::uint32_t *count, XrViewConfigurationView *values) {
            const bool quad = type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET;
            *count = quad ? active->quadViews : active->views;
            if (capacity && capacity < *count)
                return XR_ERROR_SIZE_INSUFFICIENT;
            if (capacity)
                for (std::uint32_t index = 0; index < *count; ++index) {
                    values[index].recommendedImageRectWidth = quad && index == 3 && active->invalidQuadView
                                                                  ? 4096
                                                                  : (quad ? 1024 - index * 128 : (active->invalidView ? 4096 : 1024));
                    values[index].maxImageRectWidth = 2048;
                    values[index].recommendedImageRectHeight = 1024;
                    values[index].maxImageRectHeight = 2048;
                    values[index].recommendedSwapchainSampleCount = 1;
                    values[index].maxSwapchainSampleCount = 4;
                    if (values[index].next) {
                        ++active->synthesisEyeQueries;
                        auto &synthesis = *static_cast<XrFrameSynthesisConfigViewEXT *>(values[index].next);
                        REQUIRE(synthesis.type == XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT);
                        synthesis.recommendedMotionVectorImageRectWidth =
                            active->invalidReprojectionExtent ? 4096 : 640 + index * active->synthesisEyeWidthStep;
                        synthesis.recommendedMotionVectorImageRectHeight = 512;
                    }
                }
            return Record(quad ? "quad-views" : "views");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL BlendModes(XrInstance, XrSystemId, const XrViewConfigurationType type,
                                                         const std::uint32_t capacity, std::uint32_t *count,
                                                         XrEnvironmentBlendMode *values) {
            const bool quad = type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET;
            *count = quad ? 2 : 1;
            const std::string_view phase = quad ? "quad-blend-modes" : "blend-modes";
            if (capacity < *count)
                return capacity == 0 ? Record(phase) : XR_ERROR_SIZE_INSUFFICIENT;
            values[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            if (quad)
                values[1] = active->duplicateQuadBlend ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
            return Record(phase);
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Spaces(XrSession session, const std::uint32_t capacity, std::uint32_t *count,
                                                     XrReferenceSpaceType *values) {
            CHECK(Script::active->liveSessions == 1);
            CHECK(session != XR_NULL_HANDLE);
            *count = active->spaces;
            if (capacity && capacity < *count)
                return XR_ERROR_SIZE_INSUFFICIENT;
            if (capacity && *count > 0) {
                values[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
                if (*count > 1)
                    values[1] = active->duplicateSpaces ? XR_REFERENCE_SPACE_TYPE_VIEW : XR_REFERENCE_SPACE_TYPE_LOCAL;
                if (*count > 2)
                    values[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
            }
            return Record("spaces");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Formats(XrSession session, const std::uint32_t capacity, std::uint32_t *count,
                                                      std::int64_t *values) {
            CHECK(Script::active->liveSessions == 1);
            CHECK(session != XR_NULL_HANDLE);
            *count = active->formats;
            if (capacity && capacity < *count)
                return XR_ERROR_SIZE_INSUFFICIENT;
            if (capacity)
                for (std::uint32_t index = 0; index < *count; ++index)
                    values[index] = index < active->formatPublication.size() ? active->formatPublication[index] : 37 + index;
            return Record("formats");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
            if (active->extraProc && instance != XR_NULL_HANDLE) {
                const auto extra = active->extraProc(instance, name, function);
                if (extra != XR_ERROR_FUNCTION_UNSUPPORTED)
                    return extra;
            }

            struct Entry {
                std::string_view name;
                PFN_xrVoidFunction function;
            };

            const std::array entries{Entry{"xrGetInstanceProperties", std::bit_cast<PFN_xrVoidFunction>(&Instance)},
                                     Entry{"xrGetSystemProperties", std::bit_cast<PFN_xrVoidFunction>(&System)},
                                     Entry{"xrEnumerateSpatialCapabilitiesEXT", std::bit_cast<PFN_xrVoidFunction>(&SpatialCapabilities)},
                                     Entry{"xrEnumerateSpatialCapabilityComponentTypesEXT",
                                           std::bit_cast<PFN_xrVoidFunction>(&SpatialComponents)},
                                     Entry{"xrEnumerateViewConfigurations", std::bit_cast<PFN_xrVoidFunction>(&Configurations)},
                                     Entry{"xrEnumerateViewConfigurationViews", std::bit_cast<PFN_xrVoidFunction>(&Views)},
                                     Entry{"xrEnumerateEnvironmentBlendModes", std::bit_cast<PFN_xrVoidFunction>(&BlendModes)},
                                     Entry{"xrEnumerateReferenceSpaces", std::bit_cast<PFN_xrVoidFunction>(&Spaces)},
                                     Entry{"xrEnumerateSwapchainFormats", std::bit_cast<PFN_xrVoidFunction>(&Formats)}};
            const auto found = std::ranges::find(entries, std::string_view{name}, &Entry::name);
            if (found == entries.end())
                return Script::GetProc(instance, name, function);
            *function = found->function;
            return XR_SUCCESS;
        }
    };

    /** @brief Exposes official census dispatch while retaining the shared native owner script. */
    class CensusLibrary final : public Platform::DynamicLibrary {
    public:
        void *GetSymbol(const std::string_view name) const noexcept override {
            if (name == "xrGetInstanceProcAddr")
                return std::bit_cast<void *>(&CensusScript::GetProc);
            if (name == "xrDestroyInstance")
                return std::bit_cast<void *>(&Script::DestroyInstance);
            return nullptr;
        }
    };
}  // namespace Horo::Tests::OpenXR
