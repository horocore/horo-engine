#pragma once
/** @file OpenXRNativeCensus.h
 * @brief Bounded observed native publications; these private values grant no Ready or frame authority.
 */
#include "Horo/XR/XRCapabilities.h"

#include <array>
#include <openxr/openxr.h>

namespace Horo::XR::OpenXRInternal {
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
    };

    /** @brief Session-dependent exact publications, observed only after actual native session creation. */
    struct NativeSessionCensus final {
        std::array<XrReferenceSpaceType, 16> spaces{};
        std::uint32_t spaceCount{};
        std::array<std::int64_t, 128> formats{};
        std::uint32_t formatCount{};
    };
}  // namespace Horo::XR::OpenXRInternal
