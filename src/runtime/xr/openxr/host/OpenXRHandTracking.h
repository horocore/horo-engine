#pragma once

/** @file OpenXRHandTracking.h @brief Selected OpenXR provider for simulation-cutoff hand joint evidence. */
#include "Horo/XR/XRHandTracking.h"
#include "OpenXRNativeSession.h"

namespace Horo::XR::OpenXRInternal {
    /** @brief Native hand sample normalized to Horo space/time before device generations are assigned. */
    struct NativeHandJointSample final {
        std::optional<XRPoseSample> pose;
        std::optional<float> radiusMeters;
    };

    /** @brief Fixed storage for both anatomical hands; inactive hands expose no joint evidence. */
    struct NativeHandSamples final {
        struct Hand final {
            std::array<NativeHandJointSample, XRHandTrackingHardLimits::JointsPerHand> joints{};
        };

        std::array<Hand, XRHandTrackingHardLimits::MaximumHands> hands{};
    };

    /** @brief Owns the two native trackers and a generation-fenced sampling provider.
     * @pre The application serializes preparation, sampling, and retirement on its native session owner lane.
     * Native tracker debt is retired before the selected XrSession.
     */
    class OpenXRHandTracking final {
    public:
        explicit OpenXRHandTracking(OpenXRNativeSession &owner) noexcept : native_(&owner) {}

        ~OpenXRHandTracking();
        OpenXRHandTracking(const OpenXRHandTracking &) = delete;
        OpenXRHandTracking &operator=(const OpenXRHandTracking &) = delete;

        /** @brief Creates both trackers on the prepared native session.
         * @param preparation Exact prepared attempt. @return Original lookup or creation failure, with debt retained for Close.
         */
        [[nodiscard]] Result<void> Prepare(const NativeSessionPreparation &preparation);

        /** @brief Publishes the sampling route for the exact activated session.
         * @param session Issued session candidate. @return Success or typed provider/identity failure.
         */
        [[nodiscard]] Result<void> Bind(const XRSessionId &session);

        /** @brief Locates both hands at one canonical Input cutoff using the same calibrated native space.
         * @param space Exact owner-fenced native space and typed coordinate identity. @param time Current simulation-cutoff evidence.
         * @return Complete fixed-size observation or typed stale/native/malformed evidence failure.
         */
        [[nodiscard]] Result<NativeHandSamples> Sample(XrSpace nativeSpace, const XRCoordinateSpace &space,
                                                       const XRTimeEvidence &time) const;

        /** @brief Revokes callable sampling before releasing native children.
         * @return Success or the original retryable native cleanup error.
         */
        [[nodiscard]] Result<void> Close();

    private:
        OpenXRNativeSession *native_;
        std::uint64_t incarnation_{};
        PFN_xrDestroyHandTrackerEXT destroy_{};
        std::optional<NativeFeatureProviderHandle> provider_;
        std::array<XrHandTrackerEXT, XRHandTrackingHardLimits::MaximumHands> trackers_{};
        const std::thread::id lane_{std::this_thread::get_id()};
        mutable bool active_{};
    };
}  // namespace Horo::XR::OpenXRInternal
