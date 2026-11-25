#pragma once
/** @file OpenXRNativeCensus.h
 * @brief Bounded observed native publications; these private values grant no Ready or frame authority.
 */
#include "Horo/Runtime/Render/RenderResourceDescriptors.h"
#include "Horo/XR/XRCapabilities.h"
#include "Horo/XR/XRViewRenderPlan.h"

#include <array>
#include <openxr/openxr.h>

namespace Horo::XR::OpenXRInternal {
    /** @brief Cold spatial plane support, independent of permission grants and live observations. */
    struct NativePlaneCensus final {
        XRCapabilityState state{XRCapabilityState::Unsupported};
        bool semanticLabels{};
    };

    /** @brief Finite exact system publications observed from the retained instance/system. */
    struct NativeSystemCensus final {
        XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES};
        XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
        std::array<XrViewConfigurationType, 16> configurations{};
        std::uint32_t configurationCount{};
        std::array<XrViewConfigurationView, XRHardLimits::MaximumViews> primaryViews{};
        std::uint32_t primaryViewCount{};
        std::array<XrEnvironmentBlendMode, 16> primaryBlendModes{};
        std::uint32_t primaryBlendModeCount{};
        XRCapabilityState handTracking{XRCapabilityState::Unsupported}; /**< Observed system support for the admitted EXT route. */
        NativePlaneCensus planes;
        XRCapabilityState eyeGazeProvider{XRCapabilityState::Unsupported}; /**< Hardware support from the exact native census. */
    };

    /** @brief One ordered view's cold semantic role, image extents and sample evidence. */
    struct NativeQuadView final {
        XRViewRole role{XRViewRole::Auxiliary};
        Render::FramebufferExtent recommendedExtent;
        Render::FramebufferExtent maximumExtent;
        std::uint32_t recommendedSamples{};
        std::uint32_t maximumSamples{};
    };

    /** @brief Complete native quad discovery. Availability here grants no session or submission authority. */
    struct NativeQuadViewCensus final {
        XRCapabilityState state{XRCapabilityState::Unsupported};
        std::array<NativeQuadView, 4> views{};
        std::array<XrEnvironmentBlendMode, 16> blendModes{};
        std::uint32_t blendModeCount{};
    };

    /** @brief Known native frame synthesis wire routes; discovery never grants a provider. */
    enum class NativeReprojectionRoute : std::uint8_t {
        None,
        FrameSynthesisExt,
        SpaceWarpFb
    };

    /** @brief Cold native-only evidence from the exact prepared instance/system and primary stereo profile. */
    struct NativeReprojectionOffer final {
        NativeReprojectionRoute route{NativeReprojectionRoute::None};
        std::uint32_t version{};
        XRCapabilityState availabilityWithoutProducer{XRCapabilityState::Unsupported};
        std::array<Render::FramebufferExtent, 2> extents{}; /**< Both set only after an enabled and validated native query. */
    };

    /** @brief Bounded rates from the current session's optional native enumeration. */
    struct NativeDisplayRates final {
        static constexpr std::size_t MaximumRates = 32;
        XRCapabilityState state{XRCapabilityState::Unsupported};
        std::array<std::uint32_t, MaximumRates> milliHertz{};
        std::uint32_t count{};
    };

    /** @brief Session-dependent exact publications, observed only after actual native session creation. */
    struct NativeSessionCensus final {
        std::array<XrReferenceSpaceType, 16> spaces{};
        std::uint32_t spaceCount{};
        std::array<std::int64_t, 128> formats{};
        std::uint32_t formatCount{};
        NativeDisplayRates displayRates;
    };
}  // namespace Horo::XR::OpenXRInternal
