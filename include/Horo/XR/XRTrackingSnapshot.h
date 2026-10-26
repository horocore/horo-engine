#pragma once

/**
 * @file XRTrackingSnapshot.h
 * @brief Immutable bounded XR device and simulation-tracking snapshot contract.
 */

#include "Horo/XR/XRContract.h"
#include "Horo/XR/XRSpacePose.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::XR {
    /** @brief Semantic tracked-device role; role is not device identity or a native path. */
    enum class XRTrackedDeviceRole : std::uint8_t {
        Head,
        LeftController,
        RightController,
        GenericTracker,
        Count
    };

    /** @brief Closed pose-purpose vocabulary advertised independently by each device. */
    enum class XRTrackedPoseKind : std::uint8_t {
        Head,
        Grip,
        Aim,
        Palm,
        Tracker,
        Count
    };

    /** @brief Backend-neutral capabilities exposed by one exact device generation. */
    enum class XRTrackedDeviceCapability : std::uint8_t {
        HeadPose,
        GripPose,
        AimPose,
        PalmPose,
        TrackerPose,
        Haptics,
        Count
    };

    /** @brief Current device availability without preserving a stale valid pose. */
    enum class XRTrackedDeviceState : std::uint8_t {
        Tracked,
        TrackingLost,
        Disconnected,
        Count
    };

    /** @brief Application focus evidence captured with a tracking publication. */
    enum class XRTrackingFocusState : std::uint8_t {
        Focused,
        Unfocused,
        Count
    };

    /** @brief Session visibility evidence captured with a tracking publication. */
    enum class XRTrackingVisibilityState : std::uint8_t {
        Visible,
        Hidden,
        Count
    };

    /** @brief Aggregate collection state; loss states never imply a reusable prior snapshot. */
    enum class XRTrackingState : std::uint8_t {
        Active,
        TrackingLost,
        PermissionLost,
        SessionLost,
        Count
    };

    struct XRTrackingSnapshotRevisionTag;

    /** @brief Monotonic identity of one immutable tracking publication. */
    using XRTrackingSnapshotRevision = XRGeneration<XRTrackingSnapshotRevisionTag>;

    /** @brief Fixed capability bits with no backend-native extension namespace. */
    struct XRTrackedDeviceCapabilities final {
        std::array<bool, static_cast<std::size_t>(XRTrackedDeviceCapability::Count)> values{}; /**< Known capability values. */

        /**
         * @brief Checks one known capability.
         * @param capability Capability to inspect.
         * @return True only when the known capability was advertised.
         */
        [[nodiscard]] constexpr bool Has(const XRTrackedDeviceCapability capability) const noexcept {
            return capability < XRTrackedDeviceCapability::Count && values[static_cast<std::size_t>(capability)];
        }

        constexpr auto operator<=>(const XRTrackedDeviceCapabilities &) const noexcept = default;
    };

    /** @brief One generation-safe device record retained by an immutable snapshot. */
    struct XRTrackedDeviceRecord final {
        XRDeviceId id;                                                  /**< Exact session-owned device generation. */
        XRTrackedDeviceRole role{XRTrackedDeviceRole::GenericTracker};  /**< Semantic role, not identity. */
        XRTrackedDeviceState state{XRTrackedDeviceState::Disconnected}; /**< Current availability. */
        XRTrackedDeviceCapabilities capabilities;                       /**< Finite Horo capability set. */
        constexpr auto operator<=>(const XRTrackedDeviceRecord &) const noexcept = default;
    };

    /** @brief One device-owned simulation pose in a snapshot's canonical pose order. */
    struct XRTrackedPoseRecord final {
        XRDeviceId device;                                  /**< Exact device generation that owns the pose. */
        XRTrackedPoseKind kind{XRTrackedPoseKind::Tracker}; /**< Semantic pose purpose. */
        XRPoseSample sample;                                /**< Immutable validity-aware simulation evidence. */
    };

    /** @brief Closed scalar action payload; native paths and canonical Input identities are deliberately absent. */
    enum class XRTrackedActionKind : std::uint8_t {
        Boolean,
        Scalar,
        Vector2,
        Count
    };

    /** @brief One current binding-plan entry sampled at this snapshot's simulation cutoff. */
    struct XRTrackedActionRecord final {
        XRDeviceId device;            /**< Exact session-owned physical source incarnation. */
        std::uint32_t bindingIndex{}; /**< Index into the exact binding revision, never a durable action identity. */
        XRTrackedActionKind kind{XRTrackedActionKind::Boolean}; /**< Declared native value shape. */
        bool active{};                                          /**< Inactive records carry exact zero values. */
        float x{};                                              /**< Finite normalized scalar or first vector component. */
        float y{};                                              /**< Finite normalized second component; zero for scalar/Boolean. */
        constexpr auto operator<=>(const XRTrackedActionRecord &) const noexcept = default;
    };

    /** @brief Finite admitted capacities; publication rejects rather than truncates overflow. */
    struct XRTrackingSnapshotLimits final {
        std::uint32_t maximumDevices{}; /**< Admitted device records, at most MaximumDevices. */
        std::uint32_t maximumPoses{};   /**< Admitted pose records, at most MaximumPoses. */
        std::uint32_t maximumActions{}; /**< Admitted scalar-action records; zero preserves tracking-only callers. */
        constexpr auto operator<=>(const XRTrackingSnapshotLimits &) const noexcept = default;
    };

    /** @brief Compile-time ceilings for allocation and bounded-query work. */
    struct XRTrackingSnapshotHardLimits final {
        static constexpr std::uint32_t MaximumDevices = 64;
        static constexpr std::uint32_t MaximumPoses = 320;
        static constexpr std::uint32_t MaximumActions = 256;
    };

    /** @brief Caller-owned candidate data validated before any immutable snapshot is published. */
    struct XRTrackingSnapshotDescriptor final {
        XRContractVersion contractVersion{CurrentXRContractVersion};             /**< Horo XRApi schema version. */
        XRSessionId session;                                                     /**< Exact publication owner. */
        XRTrackingSnapshotRevision revision;                                     /**< Monotonic publication revision. */
        std::uint64_t sequence{};                                                /**< Non-zero owner-issued sample sequence. */
        XRRuntimeTime sampledAt;                                                 /**< Exact runtime sampling timestamp. */
        XRWorldOriginRevision worldOriginRevision;                               /**< Exact world-origin adapter revision. */
        XRTrackingFocusState focus{XRTrackingFocusState::Unfocused};             /**< Captured application focus. */
        XRTrackingVisibilityState visibility{XRTrackingVisibilityState::Hidden}; /**< Captured session visibility. */
        XRTrackingState state{XRTrackingState::SessionLost};                     /**< Aggregate collection state. */
        XRTrackingSnapshotLimits limits;                                         /**< Admitted finite plan bounds. */
        std::span<const XRTrackedDeviceRecord> devices;                          /**< Canonically ordered devices. */
        std::span<const XRTrackedPoseRecord> poses;                              /**< Canonically ordered device poses. */
        std::uint64_t bindingRevision{};                /**< Non-zero exact resolved-plan revision whenever actions are admitted. */
        std::span<const XRTrackedActionRecord> actions; /**< Canonical (binding index, device) ordered action evidence. */
    };

    /**
     * @brief Immutable owner-backed device and simulation-tracking publication.
     *
     * Storage is copied only after all bounds and invariants pass. Access remains memory-safe after owner shutdown, while
     * liveness-sensitive use must pass ValidateXRTrackingSnapshot or QueryXRTrackedPose.
     */
    class XRTrackingSnapshot final {
    public:
        /**
         * @brief Validates and atomically copies one bounded candidate publication.
         * @param descriptor Complete candidate and admitted limits.
         * @param activeSession Current session, or invalid after shutdown.
         * @param activeOriginRevision Current world-origin adapter revision.
         * @return Immutable snapshot or typed version, identity, capacity, unsupported, stale, or invalid failure.
         * @post Successful construction and value copies require no dynamic allocation.
         * @post On failure, no partial snapshot is published and all caller-owned spans remain unmodified.
         */
        [[nodiscard]] static Result<XRTrackingSnapshot> Create(const XRTrackingSnapshotDescriptor &descriptor,
                                                               const XRSessionId &activeSession,
                                                               XRWorldOriginRevision activeOriginRevision);

        /** @brief Returns the contract version. @return Exact immutable Horo XRApi version. */
        [[nodiscard]] XRContractVersion ContractVersion() const noexcept;
        /** @brief Returns the owner session. @return Exact session generation. */
        [[nodiscard]] const XRSessionId &Session() const noexcept;
        /** @brief Returns the publication revision. @return Exact snapshot revision. */
        [[nodiscard]] XRTrackingSnapshotRevision Revision() const noexcept;
        /** @brief Returns the sample sequence. @return Non-zero owner-issued sequence. */
        [[nodiscard]] std::uint64_t Sequence() const noexcept;
        /** @brief Returns the runtime sample time. @return Exact typed timestamp. */
        [[nodiscard]] XRRuntimeTime SampledAt() const noexcept;
        /** @brief Returns the captured world-origin revision. @return Exact origin generation. */
        [[nodiscard]] XRWorldOriginRevision WorldOriginRevision() const noexcept;
        /** @brief Returns focus evidence. @return Captured focus state. */
        [[nodiscard]] XRTrackingFocusState Focus() const noexcept;
        /** @brief Returns visibility evidence. @return Captured visibility state. */
        [[nodiscard]] XRTrackingVisibilityState Visibility() const noexcept;
        /** @brief Returns aggregate tracking state. @return Captured collection state. */
        [[nodiscard]] XRTrackingState State() const noexcept;
        /** @brief Returns admitted bounds. @return Exact immutable publication limits. */
        [[nodiscard]] const XRTrackingSnapshotLimits &Limits() const noexcept;
        /** @brief Returns canonical device records. @return Immutable contiguous owned records. */
        [[nodiscard]] std::span<const XRTrackedDeviceRecord> Devices() const noexcept;
        /** @brief Returns canonical pose records. @return Immutable contiguous owned records. */
        [[nodiscard]] std::span<const XRTrackedPoseRecord> Poses() const noexcept;

        /** @brief Returns the captured binding-plan revision. @return Zero only for tracking-only publications. */
        [[nodiscard]] std::uint64_t BindingRevision() const noexcept;
        /** @brief Returns canonical action evidence. @return Immutable owned populated prefix. */
        [[nodiscard]] std::span<const XRTrackedActionRecord> Actions() const noexcept;

    private:
        [[nodiscard]] static std::array<XRTrackedPoseRecord, XRTrackingSnapshotHardLimits::MaximumPoses> EmptyPoses() noexcept;
        explicit XRTrackingSnapshot(const XRTrackingSnapshotDescriptor &descriptor);

        XRContractVersion contractVersion_;
        XRSessionId session_;
        XRTrackingSnapshotRevision revision_;
        std::uint64_t sequence_{};
        XRRuntimeTime sampledAt_;
        XRWorldOriginRevision worldOriginRevision_;
        XRTrackingFocusState focus_;
        XRTrackingVisibilityState visibility_;
        XRTrackingState state_;
        XRTrackingSnapshotLimits limits_;
        std::array<XRTrackedDeviceRecord, XRTrackingSnapshotHardLimits::MaximumDevices> devices_{};
        std::array<XRTrackedPoseRecord, XRTrackingSnapshotHardLimits::MaximumPoses> poses_ = EmptyPoses();
        std::array<XRTrackedActionRecord, XRTrackingSnapshotHardLimits::MaximumActions> actions_{};
        std::size_t deviceCount_{};
        std::size_t poseCount_{};
        std::size_t actionCount_{};
        std::uint64_t bindingRevision_{};
    };

    /**
     * @brief Validates a retained snapshot against current session, revision, and origin fences.
     * @param snapshot Immutable snapshot to admit.
     * @param activeSession Current session, or invalid after shutdown.
     * @param expectedRevision Revision retained by the consumer.
     * @param activeOriginRevision Current world-origin adapter revision.
     * @return Success or typed invalid, identity-stale, snapshot-stale, or origin-stale failure.
     */
    [[nodiscard]] Result<void> ValidateXRTrackingSnapshot(const XRTrackingSnapshot &snapshot, const XRSessionId &activeSession,
                                                          XRTrackingSnapshotRevision expectedRevision,
                                                          XRWorldOriginRevision activeOriginRevision);

    /**
     * @brief Queries one valid current simulation pose through bounded canonical storage.
     * @param snapshot Immutable snapshot to query.
     * @param device Exact requested device generation.
     * @param kind Semantic pose kind.
     * @param activeSession Current session, or invalid after shutdown.
     * @param expectedRevision Revision retained by the consumer.
     * @param activeOriginRevision Current world-origin adapter revision.
     * @return Owned pose copy or typed invalid, stale, unsupported, or unavailable failure.
     * @post Performs bounded lookup without allocation on success, blocking I/O, native calls, or state mutation.
     */
    [[nodiscard]] Result<XRPoseSample> QueryXRTrackedPose(const XRTrackingSnapshot &snapshot, const XRDeviceId &device,
                                                          XRTrackedPoseKind kind, const XRSessionId &activeSession,
                                                          XRTrackingSnapshotRevision expectedRevision,
                                                          XRWorldOriginRevision activeOriginRevision);
}  // namespace Horo::XR
