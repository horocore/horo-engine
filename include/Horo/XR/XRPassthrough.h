#pragma once

/** @file XRPassthrough.h @brief Purpose-bound authorization for an opaque runtime-composited reconstruction underlay. */
#include "Horo/XR/XRSpacePose.h"

namespace Horo::XR {
    struct XRPassthroughGrantTag;

    /** @brief Non-reusable application consent revision for runtime compositor use. */
    using XRPassthroughGrant = XRGeneration<XRPassthroughGrantTag>;

    /** @brief Exact approved session and calibrated virtual World paired with the runtime underlay. */
    struct XRPassthroughScope final {
        XRSessionId session;
        XRCoordinateSpace world;
        XRPassthroughGrant grant;
        constexpr auto operator<=>(const XRPassthroughScope &) const noexcept = default;
    };
}  // namespace Horo::XR
