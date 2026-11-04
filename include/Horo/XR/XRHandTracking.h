#pragma once
/** @file XRHandTracking.h @brief Canonical bounded articulated-hand evidence; no native joint or gesture authority. */
#include "Horo/XR/XRSpacePose.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Horo::XR {
    /** @brief Anatomical side independent of any controller role. */
    enum class XRHandedness : std::uint8_t {
        Left,
        Right,
        Count
    };
    /** @brief Closed anatomical joint order; no native enumeration is serialized. */
    enum class XRHandJoint : std::uint8_t {
        Palm,
        Wrist,
        ThumbMetacarpal,
        ThumbProximal,
        ThumbDistal,
        ThumbTip,
        IndexMetacarpal,
        IndexProximal,
        IndexIntermediate,
        IndexDistal,
        IndexTip,
        MiddleMetacarpal,
        MiddleProximal,
        MiddleIntermediate,
        MiddleDistal,
        MiddleTip,
        RingMetacarpal,
        RingProximal,
        RingIntermediate,
        RingDistal,
        RingTip,
        LittleMetacarpal,
        LittleProximal,
        LittleIntermediate,
        LittleDistal,
        LittleTip,
        Count
    };

    /** @brief Fixed anatomy/storage ceilings; providers reject overflow rather than truncate joints. */
    struct XRHandTrackingHardLimits final {
        static constexpr std::uint32_t MaximumHands = 2;
        static constexpr std::uint32_t JointsPerHand = static_cast<std::uint32_t>(XRHandJoint::Count);
        static constexpr std::uint32_t MaximumJoints = MaximumHands * JointsPerHand;
    };

    /** @brief One immutable snapshot's joint record. Missing joints are omitted, never copied from prior evidence. */
    struct XRHandJointRecord final {
        XRDeviceId device; /**< Exact hand source incarnation, never a controller alias. */
        XRHandedness hand{XRHandedness::Count};
        XRHandJoint joint{XRHandJoint::Count};
        std::optional<XRPoseSample> sample; /**< Populated records require same-cutoff simulation evidence. */
        std::optional<float> radiusMeters;  /**< Absent when unavailable; finite nonnegative SI radius otherwise. */
    };

    /** @brief Validates joint shape and exact pose/session/time/origin provenance without allocating.
     * Device role/capability, canonical order and aggregate state are validated by XRTrackingSnapshot.
     * Raw live joints remain sensitive; this contract grants no capture/export permission.
     * @param record One exact anatomical joint with immutable pose and optional radius.
     * @param session Current session against which the hand device and pose are fenced.
     * @param sampledAt Exact runtime sample timestamp selected for the snapshot.
     * @param origin Current canonical world-origin revision.
     * @return Success or typed identity, shape, time, pose or origin validation failure.
     */
    [[nodiscard]] Result<void> ValidateXRHandJointRecord(const XRHandJointRecord &record, const XRSessionId &session,
                                                         XRRuntimeTime sampledAt, XRWorldOriginRevision origin);
}  // namespace Horo::XR
