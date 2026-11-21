#pragma once

/** @file XRInteraction.h
 * @brief Bounded interaction geometry derived from one committed XR tracking cutoff.
 */
#include "Horo/XR/XRTrackingSnapshot.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace Horo::XR {
    /** @brief Tracking fences supplied by the host to a synchronous interaction consumer. */
    struct XRInteractionCutoff final {
        XRSessionId session;
        XRTrackingSnapshotRevision tracking;
        XRWorldOriginRevision origin;
        XRRuntimeTime sampledAt;
        std::uint64_t bindingRevision{};

        constexpr auto operator<=>(const XRInteractionCutoff &) const noexcept = default;
    };

    /** @brief Copies exact tracking provenance. @param snapshot Published tracking. @return Its immutable fences. */
    [[nodiscard]] XRInteractionCutoff DescribeXRInteractionCutoff(const XRTrackingSnapshot &snapshot) noexcept;

    /** @brief Advisory proximity against the runtime-owned rectangular Stage. */
    enum class XRStageBoundaryProximity : std::uint8_t {
        Inside,
        Near,
        Outside
    };

    /** @brief Uses exact Stage-relative tracked head position to classify proximity to the runtime rectangle.
     * @param snapshot Committed simulation tracking. @param cutoff Exact live consumer fence.
     * @param referenceSpaces Runtime-owned reference publication. @param warningDistanceMeters Finite, nonnegative inward margin.
     * @return Advisory condition or typed unsupported, unavailable, stale or malformed evidence.
     * @details Reads no native data and grants no authority over guardian or chaperone behavior.
     */
    [[nodiscard]] Result<XRStageBoundaryProximity> QueryXRStageBoundary(const XRTrackingSnapshot &snapshot,
                                                                        const XRInteractionCutoff &cutoff,
                                                                        const XRReferenceSpaceSnapshot &referenceSpaces,
                                                                        float warningDistanceMeters);

    /** @brief Classifies one Stage-relative head pose derived by the same runtime owner.
     * @param head Immutable simulation pose. @param referenceSpaces Exact owned runtime references.
     * @param sampledAt Current sample time. @param warningDistanceMeters Finite, nonnegative inward margin.
     * @return Advisory condition or a typed evidence failure. No position escapes the classification.
     */
    [[nodiscard]] Result<XRStageBoundaryProximity> ClassifyXRStageBoundary(const XRPoseSample &head,
                                                                           const XRReferenceSpaceSnapshot &referenceSpaces,
                                                                           XRRuntimeTime sampledAt, float warningDistanceMeters);

    /** @brief Controller pose choices that can supply interaction geometry. */
    enum class XRControllerInteractionPose : std::uint8_t {
        Aim,
        Grip
    };

    /** @brief Selects a controller pose or the exact anatomy of an admitted hand. */
    using XRInteractionPose = std::variant<XRControllerInteractionPose, XRHandJoint>;

    /** @brief A directed segment specified by its consumer in the selected pose's local axes. */
    struct XRInteractionRayProbe final {
        Math::Vec3 localDirection;
        float maximumDistanceMeters{};
    };

    /** @brief Point probe for downstream contact testing. */
    struct XRInteractionDirectProbe final {};

    /** @brief A spherical proximity probe with a consumer-supplied radius in metres. */
    struct XRInteractionProximityProbe final {
        float radiusMeters{};
    };

    /** @brief One requested geometry kind. */
    using XRInteractionProbeShape = std::variant<XRInteractionRayProbe, XRInteractionDirectProbe, XRInteractionProximityProbe>;

    /** @brief One exact controller or hand probe, with an explicit confidence floor. */
    struct XRInteractionProbe final {
        XRDeviceId device;
        XRInteractionPose pose;
        XRInteractionProbeShape shape;
        XRTrackingConfidence minimumConfidence{XRTrackingConfidence::None};
    };

    /** @brief Ray segment, direct point, or spherical volume in one exact World space. */
    using XRInteractionShape = std::variant<Math::Ray, Math::Vec3, Math::BoundingSphere>;

    /** @brief One copied candidate. Targets, contact, occlusion and interaction semantics belong to the consumer. */
    struct XRInteractionCandidate final {
        XRDeviceId device;
        XRInteractionPose pose;
        XRCoordinateSpace space;
        XRTrackingConfidence confidence{XRTrackingConfidence::None};
        XRInteractionShape shape;
    };

    /** @brief Bounded owned geometry. Use only within the admitted host cutoff. Treat as sensitive live tracking. */
    class XRInteractionCandidates final {
    public:
        static constexpr std::size_t MaximumProbes = XRTrackingSnapshotHardLimits::MaximumDevices;

        /** @return The exact queried tracking publication. */
        [[nodiscard]] const XRInteractionCutoff &Cutoff() const noexcept {
            return cutoff_;
        }

        /** @return Populated candidates in request order. */
        [[nodiscard]] std::span<const XRInteractionCandidate> Candidates() const noexcept {
            return std::span{values_}.first(count_);
        }

    private:
        friend Result<XRInteractionCandidates> BuildXRInteractionCandidates(const XRTrackingSnapshot &, const XRInteractionCutoff &,
                                                                            std::span<const XRInteractionProbe>);
        XRInteractionCutoff cutoff_;
        std::array<XRInteractionCandidate, MaximumProbes> values_{};
        std::size_t count_{};
    };

    /** @brief Converts one current focused simulation sample into bounded interaction geometry.
     * @param snapshot The committed immutable tracking publication of the selected host.
     * @param cutoff Exact current host fence, with invalid session after retirement.
     * @param probes Exact live devices, semantic pose selectors and supplied query bounds.
     * @return One candidate per probe or a typed stale, unavailable, unsupported, invalid or capacity error.
     * @post Success requires no allocation, blocking I/O or native calls. Failure publishes no partial batch.
     */
    [[nodiscard]] Result<XRInteractionCandidates> BuildXRInteractionCandidates(const XRTrackingSnapshot &snapshot,
                                                                               const XRInteractionCutoff &cutoff,
                                                                               std::span<const XRInteractionProbe> probes);
}  // namespace Horo::XR
