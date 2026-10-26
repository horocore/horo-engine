#pragma once

/**
 * @file XRSpacePose.h
 * @brief Typed XR coordinate spaces, time domains, pose validity, and bounded composition.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/XR/XRIdentity.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace Horo::XR {
    /** @brief Horo XR coordinates use metres in right-handed, Y-up, negative-Z-forward scene space. */
    enum class XRCoordinateConvention : std::uint8_t {
        RightHandedYUpNegativeZForward,
        LeftHandedYUpPositiveZForward,
        Count
    };

    /** @brief Closed semantic reference-space vocabulary independent of native runtime enums. */
    enum class XRSpaceKind : std::uint8_t {
        View,
        Local,
        Stage,
        World,
        Action, /**< Native action-owned grip/aim space; never an alias for a reference or world space. */
        Count
    };

    /** @brief Independent validity provenance for one pose component. */
    enum class XRPoseComponentValidity : std::uint8_t {
        Invalid,
        Inferred,
        Tracked,
        Count
    };

    /** @brief Finite confidence category derived from runtime evidence, never an invented probability. */
    enum class XRTrackingConfidence : std::uint8_t {
        None,
        Low,
        High,
        Count
    };

    /** @brief Explicit aggregate tracking-loss state carried without preserving stale components. */
    enum class XRTrackingLossState : std::uint8_t {
        None,
        PositionLost,
        OrientationLost,
        FullyLost,
        Count
    };

    /** @brief Declares whether a sample is simulation evidence or a presentation prediction. */
    enum class XRPosePurpose : std::uint8_t {
        SimulationInput,
        PresentationPrediction,
        Count
    };

    struct XRSimulationTimeTag;

    /** @brief Strong domain-local timestamp; only simulation admits the canonical zero epoch.
     * @details Runtime and prediction timestamps remain strictly positive. Simulation is elapsed fixed-step time:
     * zero is a valid first/paused cutoff, while its negative default representation remains invalid.
     * These values cannot convert clock domains or issue simulation ticks.
     */
    template <typename Tag> class XRTimePoint final {
    public:
        /** @brief Constructs the reserved invalid timestamp. */
        XRTimePoint() = default;

        /**
         * @brief Creates a typed monotonic timestamp.
         * @param nanoseconds Domain-local nanoseconds; nonnegative for simulation, strictly positive otherwise.
         * @return Typed value or XRErrors::OperationInvalid.
         */
        [[nodiscard]] static Result<XRTimePoint> Create(const std::int64_t nanoseconds) {
            if (nanoseconds < 0 || (!std::is_same_v<Tag, XRSimulationTimeTag> && nanoseconds == 0))
                return Result<XRTimePoint>::Failure(MakeError(XRErrors::OperationInvalid));
            XRTimePoint value;
            value.nanoseconds_ = nanoseconds;
            return Result<XRTimePoint>::Success(value);
        }

        /** @brief Checks representation only. @return True for an admitted domain-local timestamp, including simulation zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return std::is_same_v<Tag, XRSimulationTimeTag> ? nanoseconds_ >= 0 : nanoseconds_ > 0;
        }

        /** @brief Returns the exact domain-local timestamp.
         * @return Exact representation: simulation default-invalid is -1, valid simulation starts at zero;
         * runtime/prediction default-invalid is zero and their valid values are positive.
         */
        [[nodiscard]] constexpr std::int64_t Nanoseconds() const noexcept {
            return nanoseconds_;
        }

        constexpr auto operator<=>(const XRTimePoint &) const noexcept = default;

    private:
        std::int64_t nanoseconds_{std::is_same_v<Tag, XRSimulationTimeTag> ? -1 : 0};
    };

    struct XRRuntimeTimeTag;
    struct XRRenderPredictionTimeTag;
    struct XRWorldOriginRevisionTag;

    /** @brief Timestamp in the selected XR runtime clock domain. */
    using XRRuntimeTime = XRTimePoint<XRRuntimeTimeTag>;
    /** @brief Elapsed authoritative fixed-simulation time; epoch zero is valid and paused cutoffs may repeat it. */
    using XRSimulationTime = XRTimePoint<XRSimulationTimeTag>;
    /** @brief Timestamp in the render prediction clock domain. */
    using XRRenderPredictionTime = XRTimePoint<XRRenderPredictionTimeTag>;
    /** @brief Monotonic revision of the world-origin adapter used by an XR space. */
    using XRWorldOriginRevision = XRGeneration<XRWorldOriginRevisionTag>;

    /** @brief Explicit clock evidence; time domains cannot be substituted for one another. */
    struct XRTimeEvidence final {
        XRRuntimeTime runtimeSample;                            /**< Native sample normalized into the runtime clock domain. */
        std::optional<XRSimulationTime> simulation;             /**< Present only for committed simulation-input evidence. */
        std::optional<XRRenderPredictionTime> renderPrediction; /**< Present only for presentation prediction. */
        constexpr auto operator<=>(const XRTimeEvidence &) const noexcept = default;
    };

    /** @brief Generation-safe semantic space bound to one exact world-origin revision. */
    struct XRCoordinateSpace final {
        XRSpaceId id;                              /**< Session-owned live space identity. */
        XRSpaceKind kind{XRSpaceKind::Local};      /**< Backend-neutral reference-space meaning. */
        XRWorldOriginRevision worldOriginRevision; /**< Exact origin adapter revision. */
        XRCoordinateConvention convention{XRCoordinateConvention::RightHandedYUpNegativeZForward}; /**< Horo convention. */
        constexpr auto operator<=>(const XRCoordinateSpace &) const noexcept = default;
    };

    /** @brief One optional pose value paired with its independent validity provenance. */
    template <typename Value> struct XRPoseComponent final {
        std::optional<Value> value;                                         /**< Present only for inferred or tracked evidence. */
        XRPoseComponentValidity validity{XRPoseComponentValidity::Invalid}; /**< Independent component validity. */
        constexpr bool operator==(const XRPoseComponent &) const = default;
    };

    /** @brief Backend-neutral pose and optional velocity evidence in Horo units. */
    struct XRPoseComponents final {
        XRPoseComponent<Math::Vec3> positionMeters;                  /**< Translation in metres. */
        XRPoseComponent<Math::Quaternion> orientation;               /**< Right-handed orientation. */
        XRPoseComponent<Math::Vec3> linearVelocityMetersPerSecond;   /**< Optional linear velocity. */
        XRPoseComponent<Math::Vec3> angularVelocityRadiansPerSecond; /**< Optional angular velocity. */
        XRTrackingConfidence confidence{XRTrackingConfidence::None}; /**< Finite evidence category. */
        XRTrackingLossState loss{XRTrackingLossState::FullyLost};    /**< Explicit aggregate loss state. */
        constexpr bool operator==(const XRPoseComponents &) const = default;
    };

    /** @brief Mutable construction carrier validated before an immutable located pose is published. */
    struct XRPoseDescriptor final {
        XRSessionId session;                                   /**< Exact owner session. */
        XRCoordinateSpace source;                              /**< Space whose pose is being located. */
        XRCoordinateSpace target;                              /**< Space in which the pose is expressed. */
        XRPosePurpose purpose{XRPosePurpose::SimulationInput}; /**< Simulation or presentation authority. */
        XRTimeEvidence time;                                   /**< Explicit non-interchangeable clock evidence. */
        XRPoseComponents components;                           /**< Independently validity-tagged values. */
    };

    /** @brief Immutable generation-fenced located pose with no native handles or hidden liveness. */
    class XRPoseSample final {
    public:
        /**
         * @brief Validates and copies one pose for the current session and origin revision.
         * @param descriptor Complete candidate evidence.
         * @param activeSession Current session, or invalid after shutdown.
         * @param activeOriginRevision Current world-origin adapter revision.
         * @return Immutable pose or a typed invalid, stale, space, time, or pose failure.
         */
        [[nodiscard]] static Result<XRPoseSample> Create(const XRPoseDescriptor &descriptor, const XRSessionId &activeSession,
                                                         XRWorldOriginRevision activeOriginRevision);

        /** @brief Returns the owning session. @return Exact immutable session identity. */
        [[nodiscard]] const XRSessionId &Session() const noexcept;
        /** @brief Returns the source space. @return Exact immutable source descriptor. */
        [[nodiscard]] const XRCoordinateSpace &Source() const noexcept;
        /** @brief Returns the target space. @return Exact immutable target descriptor. */
        [[nodiscard]] const XRCoordinateSpace &Target() const noexcept;
        /** @brief Returns the sample purpose. @return Simulation or presentation prediction. */
        [[nodiscard]] XRPosePurpose Purpose() const noexcept;
        /** @brief Returns clock-domain evidence. @return Immutable typed timestamps. */
        [[nodiscard]] const XRTimeEvidence &Time() const noexcept;
        /** @brief Returns pose evidence. @return Immutable independently valid components. */
        [[nodiscard]] const XRPoseComponents &Components() const noexcept;

    private:
        // Only snapshot capacity storage may hold inert entries outside its published prefix.
        friend class XRTrackingSnapshot;
        explicit XRPoseSample(const XRPoseDescriptor &descriptor) noexcept;

        XRSessionId session_;
        XRCoordinateSpace source_;
        XRCoordinateSpace target_;
        XRPosePurpose purpose_;
        XRTimeEvidence time_;
        XRPoseComponents components_;
    };

    /** @brief Hard frame-hot bound for explicit coordinate conversion chains. */
    struct XRCoordinateHardLimits final {
        static constexpr std::size_t MaximumTransformHops = 8;
    };

    /**
     * @brief Composes a contiguous bounded chain without allocation or native work.
     * @param transforms Source-to-target located transforms in traversal order.
     * @param activeSession Current owner session, or invalid after shutdown.
     * @param activeOriginRevision Current world-origin adapter revision.
     * @return Composed located pose, or typed invalid, stale, incompatible, or capacity failure.
     */
    [[nodiscard]] Result<XRPoseSample> ComposeXRSpaceTransforms(std::span<const XRPoseSample> transforms, const XRSessionId &activeSession,
                                                                XRWorldOriginRevision activeOriginRevision);
}  // namespace Horo::XR
