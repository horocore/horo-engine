#pragma once

/** @file XRPlaneTracking.h
 * @brief Bounded, purpose-scoped plane observations with exact lifetime and cadence fences.
 */
#include "Horo/Foundation/Time.h"
#include "Horo/XR/XRSpacePose.h"

#include <array>
#include <optional>
#include <span>

namespace Horo::XR {
    struct XRPlaneTag;
    struct XRPlaneGrantTag;

    /** @brief Provider-issued session-local plane identity, with no native handles. */
    using XRPlaneId = XRSessionObjectId<XRPlaneTag>;
    /** @brief One application-issued authorization for live plane observations. */
    using XRPlaneGrant = XRGeneration<XRPlaneGrantTag>;

    /** @brief Portable semantic labels for spatial surfaces. */
    enum class XRPlaneClassification : std::uint8_t {
        Unknown,
        Floor,
        Ceiling,
        Wall,
        Platform,
        Count
    };

    /** @brief Pose validity and classification are independent and optional to consumers. */
    struct XRPlaneObservation final {
        XRPlaneId id;
        Math::Vec3 centerMeters;
        Math::Quaternion orientation{Math::Quaternion::Identity()};
        Math::Vec2 extentMeters; /**< Full dimensions along the plane's local X and Z axes. */
        XRPlaneClassification classification{XRPlaneClassification::Unknown};
        XRTrackingConfidence confidence{XRTrackingConfidence::None};
    };

    /** @brief Exact retained session, coordinate origin and scope of authorized use. */
    struct XRPlaneScope final {
        XRSessionId session;
        XRCoordinateSpace space;
        XRPlaneGrant grant;
        constexpr auto operator<=>(const XRPlaneScope &) const noexcept = default;
    };

    /** @brief Caller-admitted finite capacity, refresh interval and lifetime in the runtime clock domain. */
    struct XRPlaneLimits final {
        std::uint32_t maximumPlanes{};
        Duration minimumUpdateInterval;
        Duration maximumAge;
    };

    /** @brief One immutable copied batch of live spatial evidence. This is not a persistent mesh or recording. */
    class XRPlaneSnapshot final {
    public:
        static constexpr std::size_t MaximumPlanes = 64;

        /** @return Exact authorization and coordinate scope that produced this batch. */
        [[nodiscard]] const XRPlaneScope &Scope() const noexcept {
            return scope_;
        }

        /** @return Runtime-clock location time supplied by the admitted provider. */
        [[nodiscard]] XRRuntimeTime SampledAt() const noexcept {
            return sampledAt_;
        }

        /** @return The complete validated batch in increasing plane identity order. */
        [[nodiscard]] std::span<const XRPlaneObservation> Planes() const noexcept {
            return std::span{planes_}.first(count_);
        }

    private:
        friend class XRPlanePublication;
        XRPlaneScope scope_;
        XRRuntimeTime sampledAt_;
        std::array<XRPlaneObservation, MaximumPlanes> planes_{};
        std::uint32_t count_{};
    };

    /**
     * @brief Synchronous owner-lane publication for one live plane-provider authorization.
     * @details The application must admit provider capability and purpose-bound consent before activation.
     * Tracking, permission, origin or session loss closes the publication before downstream consumers run.
     * No native discovery, logging, persistent history or sensor calls occur here.
     */
    class XRPlanePublication final {
    public:
        XRPlanePublication() = default;
        XRPlanePublication(const XRPlanePublication &) = delete;
        XRPlanePublication &operator=(const XRPlanePublication &) = delete;

        /** @brief Activates one exact authorized producer.
         * @param scope Application-issued grant and current coordinate identity.
         * @param limits The declared finite producer and consumer policy.
         * @return Complete new authorization or typed invalid, stale, busy or capacity failure. */
        [[nodiscard]] Result<void> Activate(const XRPlaneScope &scope, const XRPlaneLimits &limits);

        /** @brief Checks whether provider work may be scheduled at this runtime time.
         * @param scope The exact currently authorized scope.
         * @param now Current runtime-clock time for the selected session.
         * @return True when due, false within the required interval, or typed stale or invalid failure. */
        [[nodiscard]] Result<bool> IsDue(const XRPlaneScope &scope, XRRuntimeTime now) const;

        /** @brief Atomically publishes a complete provider result.
         * @param scope The exact active scope captured before provider work.
         * @param sampledAt Runtime-clock time when plane poses were located.
         * @param planes Complete ordered batch. An empty result withdraws previously observed planes.
         * @return Success or typed stale, invalid, rate or capacity failure. Rejection retains the previous batch.
         * @note A valid batch takes bounded work and performs no allocation on success. */
        [[nodiscard]] Result<void> Publish(const XRPlaneScope &scope, XRRuntimeTime sampledAt, std::span<const XRPlaneObservation> planes);

        /** @brief Copies the current complete batch while the exact scope and sample are eligible.
         * @param scope The exact currently authorized scope.
         * @param now Current clock used to enforce the age limit.
         * @return Owned immutable batch or typed unavailable, stale or invalid failure. */
        [[nodiscard]] Result<XRPlaneSnapshot> Read(const XRPlaneScope &scope, XRRuntimeTime now) const;

        /** @brief Withdraws the active grant and all retained observations, including an empty publication. */
        void Revoke() noexcept;

    private:
        XRPlaneScope active_;
        XRPlaneGrant lastGrant_;
        XRPlaneLimits limits_;
        std::optional<XRPlaneSnapshot> current_;
    };
}  // namespace Horo::XR
