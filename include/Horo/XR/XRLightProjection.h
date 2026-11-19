#pragma once

/** @file XRLightProjection.h @brief Scoped conversion of transient XR estimates into scene-linear renderer light values. */

#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Render/SceneColorEncoding.h"
#include "Horo/XR/XRLightEstimate.h"

namespace Horo::XR {
    /** @brief Explicit interpretation of the provider's direction in calibrated World coordinates. */
    enum class XRLightDirectionConvention : std::uint8_t {
        Unspecified,
        TowardScene,
        TowardSource,
    };

    /** @brief Host-selected relative scale, color interpretation, and bounded renderer admission. */
    struct XRLightProjectionPolicy final {
        Render::SceneColorSourcePolicy ambientColor;
        Render::SceneColorSourcePolicy directionalColor;
        XRLightDirectionConvention direction{};
        float ambientScale{};                /**< Converts provider-relative intensity into the admitted scene contribution. */
        float directionalScale{};            /**< Converts provider-relative intensity into the admitted scene contribution. */
        float maximumAbsoluteChannel{};      /**< Finite positive ceiling for each converted scene-linear channel. */
        std::uint64_t directionalIdentity{}; /**< Renderer-issued nonzero identity in the output scene light domain. */
        Duration maximumAge;
        Duration smoothingWindow; /**< Linear blend reaches the new sample after this many sample-time units. Zero disables it. */
    };

    /** @brief Owned per-frame environment lighting values in scene-linear ACEScg. */
    struct XRProjectedLighting final {
        std::optional<Math::Vec3> ambient;
        std::optional<Render::RenderLight> directional;
    };

    /** @brief Bounded presentation owner for exactly one live light-estimation grant. */
    class XRLightProjection final {
    public:
        /** @brief Validates the exact World scope and explicit renderer policy.
         * @param scope Application-issued active session, World origin and permission grant.
         * @param policy Explicit calibration and bounded contribution policy selected by the host.
         * @return Empty projection owner or actionable policy/identity failure.
         */
        [[nodiscard]] static Result<XRLightProjection> Create(XRLightEstimateScope scope, XRLightProjectionPolicy policy);

        /** @brief Projects a live observation into an owned presentation value.
         * @param active Current grant and calibrated World identity from the source owner.
         * @param now Current time from that session's runtime clock.
         * @param estimate Current observation, or nullptr when the source has withdrawn it.
         * @return Current projected lighting, empty for withdrawal, or a typed failure.
         * @pre Owner lane. A grant or origin replacement requires a fresh projection owner.
         * @post Failures and withdrawal clear the held output. A repeated timestamp can withdraw components but cannot
         * restore them or extend their lifetime. Successful updates allocate no memory, invoke no native calls, and never edit authored
         * state.
         */
        [[nodiscard]] Result<std::optional<XRProjectedLighting>> Update(const XRLightEstimateScope &active, XRRuntimeTime now,
                                                                        const XRLightEstimate *estimate);

    private:
        /** @brief Retains validated scope and conversion policy without a live observation. */
        XRLightProjection(XRLightEstimateScope scope, XRLightProjectionPolicy policy) noexcept;

        XRLightEstimateScope scope_;
        XRLightProjectionPolicy policy_;
        XRRuntimeTime lastClock_;
        XRRuntimeTime lastSample_;
        std::optional<XRProjectedLighting> current_;
    };
}  // namespace Horo::XR
