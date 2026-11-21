#pragma once
/** @file XRHandTracking.h @brief Canonical bounded articulated-hand evidence; no native joint or gesture authority. */
#include "Horo/XR/XRSpacePose.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

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
    class XRTrackingSnapshot;
    class IXRHandGesturePolicy;
    struct XRTrackingSnapshotRevisionTag;

    /** @brief Derived observation only; the policy assigns no gameplay or input meaning. */
    enum class XRHandGestureKind : std::uint8_t {
        Pinch,
        Count
    };

    /** @brief Exact classified hand and source revision. */
    struct XRHandGestureCandidate final {
        XRDeviceId device;
        XRHandedness hand{XRHandedness::Count};
        XRHandGestureKind kind{XRHandGestureKind::Count};
        XRTrackingConfidence confidence{XRTrackingConfidence::None};
    };

    /** @brief Bounded classifications for one immutable tracking revision. */
    class XRHandGestureCandidates final {
    public:
        /** @brief Borrows the candidates in canonical hand order. @return At most one per hand. */
        [[nodiscard]] std::span<const XRHandGestureCandidate> Candidates() const noexcept {
            return std::span{values_}.first(count_);
        }

    private:
        friend Result<XRHandGestureCandidates> DeriveXRHandGestures(const XRTrackingSnapshot &, const XRSessionId &,
                                                                    XRGeneration<XRTrackingSnapshotRevisionTag>, XRWorldOriginRevision,
                                                                    const IXRHandGesturePolicy &);
        std::array<XRHandGestureCandidate, XRHandTrackingHardLimits::MaximumHands> values_{};
        std::size_t count_{};
    };

    /** @brief Replaceable owner-lane classification policy; never writes tracking or routing state. */
    class IXRHandGesturePolicy {
    public:
        virtual ~IXRHandGesturePolicy() = default;
        /** @brief Classifies an immutable, canonical hand at one simulation cutoff.
         * @param joints Detached records for one active hand, borrowed only through this call.
         * @return A confidence-tagged candidate or absence. Policy execution must avoid allocation and blocking work.
         */
        [[nodiscard]] virtual std::optional<std::pair<XRHandGestureKind, XRTrackingConfidence>> Classify(
            std::span<const XRHandJointRecord> joints) const noexcept = 0;
    };

    /** @brief Uses thumb/index joint contact and tracked-tip confidence to classify a pinch. */
    class XRContactPinchGesturePolicy final : public IXRHandGesturePolicy {
    public:
        /** @copydoc IXRHandGesturePolicy::Classify */
        [[nodiscard]] std::optional<std::pair<XRHandGestureKind, XRTrackingConfidence>> Classify(
            std::span<const XRHandJointRecord> joints) const noexcept override;
    };

    /** @brief Evaluates a typed policy through the exact focused snapshot fence.
     * @param snapshot Immutable joint evidence. @param session Current active owner.
     * @param revision Current tracking revision. @param origin Current origin revision. @param policy Composed classifier.
     * @return Bounded candidates, empty when unfocused, or typed stale/invalid evidence failure.
     */
    [[nodiscard]] Result<XRHandGestureCandidates> DeriveXRHandGestures(const XRTrackingSnapshot &snapshot, const XRSessionId &session,
                                                                       XRGeneration<XRTrackingSnapshotRevisionTag> revision,
                                                                       XRWorldOriginRevision origin, const IXRHandGesturePolicy &policy);
}  // namespace Horo::XR
