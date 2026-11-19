#pragma once

/** @file XRLightEstimate.h @brief Transient, purpose-scoped XR lighting evidence. */

#include "Horo/Foundation/Time.h"
#include "Horo/XR/XRSpacePose.h"

#include <cstdint>
#include <optional>

namespace Horo::XR {
    struct XRLightEstimateGrantTag;

    /** @brief Application-issued one-use authorization for one session's lighting observations. */
    using XRLightEstimateGrant = XRGeneration<XRLightEstimateGrantTag>;

    /** @brief Generation-bound purpose grant and calibrated direction basis. */
    struct XRLightEstimateScope final {
        XRSessionId session;
        XRCoordinateSpace space;
        XRLightEstimateGrant grant;

        /** @brief Checks representation and exact World/session lineage. @return True for a well-formed scope. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return session.IsValid() && grant.IsValid() && space.id.IsValid() && space.id.session == session &&
                   space.kind == XRSpaceKind::World && space.worldOriginRevision.IsValid() &&
                   space.convention == XRCoordinateConvention::RightHandedYUpNegativeZForward;
        }

        constexpr auto operator<=>(const XRLightEstimateScope &) const noexcept = default;
    };

    /** @brief Runtime-clock sampling and expiry limits chosen by the approved consumer. */
    struct XRLightEstimateLimits final {
        Duration minimumUpdateInterval;
        Duration maximumAge;
    };

    /** @brief Source contract for the relative RGB channels. */
    enum class XRLightEstimateProvenance : std::uint8_t {
        AndroidOpenXR,
    };

    /** @brief Finite validity evidence; the Android provider publishes a flag, with no probability. */
    enum class XRLightEstimateConfidence : std::uint8_t {
        ProviderValid,
    };

    /** @brief Finite nonnegative provider-relative RGB without declared canonical primaries or physical units. */
    struct XRLightEstimateIntensity final {
        Math::Vec3 relativeRgb;
        XRLightEstimateConfidence confidence{XRLightEstimateConfidence::ProviderValid};
    };

    /** @brief Optional dominant light estimate with a unit direction in the exact queried World axes. */
    struct XRDirectionalLightEstimate final {
        XRLightEstimateIntensity intensity;
        Math::Vec3 providerDirection; /**< Direction as defined by the provider; renderer convention requires explicit projection. */
    };

    /** @brief At most one ambient and one dominant directional observation in one live provider sample. */
    struct XRLightEstimate final {
        XRLightEstimateScope scope;
        XRRuntimeTime sampledAt;
        XRLightEstimateProvenance provenance{XRLightEstimateProvenance::AndroidOpenXR};
        std::optional<XRLightEstimateIntensity> ambient;
        std::optional<XRDirectionalLightEstimate> directional;
    };
}  // namespace Horo::XR
