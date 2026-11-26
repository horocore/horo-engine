#pragma once

/** @file OpenXRViewRecommendations.h @brief Private instance-scoped OpenXR view recommendations. */

#include <array>
#include <cstddef>
#include <cstdint>
#include <openxr/openxr.h>
#include <optional>

namespace Horo::XR::OpenXRInternal {
    /** @brief Recommended render dimensions for one view in native enumeration order. */
    struct OpenXRRecommendedViewExtent final {
        std::uint32_t width{};
        std::uint32_t height{};
    };

    using OpenXRPrimaryStereoExtents = std::array<OpenXRRecommendedViewExtent, 2>;

    /** @brief Validates bounded values without implying renderer or session admission. */
    [[nodiscard]] inline bool ValidNativeViewRecommendation(const XrViewConfigurationView &view) noexcept {
        return view.type == XR_TYPE_VIEW_CONFIGURATION_VIEW && !view.next && view.recommendedImageRectWidth > 0 &&
               view.recommendedImageRectHeight > 0 && view.recommendedSwapchainSampleCount > 0 &&
               view.recommendedImageRectWidth <= view.maxImageRectWidth && view.recommendedImageRectHeight <= view.maxImageRectHeight &&
               view.recommendedSwapchainSampleCount <= view.maxSwapchainSampleCount;
    }

    /** @brief Reads exactly two ordered primary-stereo recommendations from a retained instance and system.
     * @param enumerate Exact loader-validated enumeration dispatch.
     * @param instance Live OpenXR instance.
     * @param system HMD system selected from that instance.
     * @return A complete valid stereo observation, or no evidence on any failure.
     */
    [[nodiscard]] inline std::optional<OpenXRPrimaryStereoExtents> ReadPrimaryStereoExtents(PFN_xrEnumerateViewConfigurationViews enumerate,
                                                                                            XrInstance instance,
                                                                                            XrSystemId system) noexcept {
        if (!enumerate || instance == XR_NULL_HANDLE || system == XR_NULL_SYSTEM_ID)
            return std::nullopt;
        constexpr auto configuration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        std::array<XrViewConfigurationView, 2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        std::uint32_t count{};
        if (enumerate(instance, system, configuration, 0, &count, nullptr) != XR_SUCCESS || count != views.size())
            return std::nullopt;
        if (enumerate(instance, system, configuration, static_cast<std::uint32_t>(views.size()), &count, views.data()) != XR_SUCCESS ||
            count != views.size())
            return std::nullopt;
        OpenXRPrimaryStereoExtents result{};
        for (std::size_t index = 0; index < views.size(); ++index) {
            const auto &view = views[index];
            if (!ValidNativeViewRecommendation(view))
                return std::nullopt;
            result[index] = {view.recommendedImageRectWidth, view.recommendedImageRectHeight};
        }
        return result;
    }
}  // namespace Horo::XR::OpenXRInternal
