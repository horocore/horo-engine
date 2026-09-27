#pragma once

/**
 * @file XRCapabilities.h
 * @brief Immutable versioned XR system capability and finite limit contract.
 */

#include "Horo/XR/XRContract.h"
#include "Horo/XR/XRIdentity.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Horo::XR {
    /** @brief Closed version-one backend-neutral XR capability vocabulary. */
    enum class XRCapability : std::uint8_t {
        Projection,
        OrientationTracking,
        PositionTracking,
        ViewSpace,
        LocalSpace,
        StageSpace,
        BooleanActions,
        FloatActions,
        Vector2Actions,
        PoseActions,
        Haptics,
        DepthComposition,
        FixedFoveation,
        RefreshRateSelection,
        PrimaryOpaqueStereo,
        PredictedFrames,
        ExternalColorTargets,
        SessionLossLifecycle,
        CanonicalInputProjection,
        TrackedControllerRoles,
        ControllerAimGrip,
        SuggestedBindings,
        CancellableHaptics,
        RuntimeUiInteraction,
        ComfortPolicy,
        VisibilityMask,
        ControllerPresentation,
        AndroidStandalone,
        Passthrough,
        EyeGaze,
        QuadViews,
        SpaceWarp,
        Count
    };

    /** @brief Explicit support and availability evidence; discovery never collapses to a boolean. */
    enum class XRCapabilityState : std::uint8_t {
        Unsupported,
        Unavailable,
        Available,
        PermissionRequired,
        Denied,
        DependencyMissing,
        Incompatible,
        TemporarilyUnavailable,
        Lost,
        DisabledByPolicy,
        Count
    };

    /** @brief Compile-time storage ceilings that prevent runtime evidence from creating unbounded frame-hot work. */
    struct XRHardLimits final {
        static constexpr std::uint32_t MaximumViews = 16;    /**< Absolute view-set storage ceiling. */
        static constexpr std::uint32_t MaximumSpaces = 256;  /**< Absolute live-space storage ceiling. */
        static constexpr std::uint32_t MaximumActions = 512; /**< Absolute live-action storage ceiling. */
        static constexpr std::uint32_t MaximumDevices = 64;  /**< Absolute live-device storage ceiling. */
    };

    /** @brief Finite system capacities declared by one exact immutable capability publication. */
    struct XRSystemLimits final {
        std::uint32_t maximumViews{};   /**< Maximum views in one published view set. */
        std::uint32_t maximumSpaces{};  /**< Maximum concurrently live Horo space identities. */
        std::uint32_t maximumActions{}; /**< Maximum concurrently live Horo action identities. */
        std::uint32_t maximumDevices{}; /**< Maximum concurrently live Horo device identities. */

        constexpr auto operator<=>(const XRSystemLimits &) const noexcept = default;
    };

    /** @brief Mutable construction input validated before becoming an immutable snapshot. */
    struct XRCapabilityDescriptor final {
        XRContractVersion contractVersion{CurrentXRContractVersion};                           /**< Horo XRApi schema version. */
        XRSystemId system;                                                                     /**< Exact owner of all evidence. */
        XRCapabilityRevision revision;                                                         /**< Non-zero atomic publication revision. */
        std::array<XRCapabilityState, static_cast<std::size_t>(XRCapability::Count)> states{}; /**< Closed evidence table. */
        XRSystemLimits limits{}; /**< Finite capacities associated with this exact evidence. */
    };

    /** @brief Bounded requested capacity for one operation; zeros mean that resource family is not requested. */
    struct XRCapabilityRequirement final {
        XRCapability capability{XRCapability::Projection}; /**< Exact capability required; no fallback is inferred. */
        std::uint32_t views{};                             /**< Requested view capacity, or zero when unused. */
        std::uint32_t spaces{};                            /**< Requested live-space capacity, or zero when unused. */
        std::uint32_t actions{};                           /**< Requested live-action capacity, or zero when unused. */
        std::uint32_t devices{};                           /**< Requested live-device capacity, or zero when unused. */
    };

    /**
     * @brief Immutable owner-backed XR capability and limit evidence.
     *
     * The snapshot has fixed size, owns no native values, performs no discovery and remains readable after owner shutdown.
     * Liveness-sensitive operations must still validate its exact system and revision through AdmitXRCapability.
     */
    class XRCapabilitySnapshot final {
    public:
        /**
         * @brief Validates and atomically forms one immutable value snapshot.
         * @param descriptor Complete exact-system capability evidence.
         * @return Snapshot or a typed invalid-version, incompatible-version, or invalid-descriptor result.
         */
        [[nodiscard]] static Result<XRCapabilitySnapshot> Create(const XRCapabilityDescriptor &descriptor);

        /** @brief Returns the Horo XRApi contract version. @return Immutable backend-neutral version. */
        [[nodiscard]] XRContractVersion ContractVersion() const noexcept;
        /** @brief Returns the system that owns the evidence. @return Exact generation-safe system identity. */
        [[nodiscard]] const XRSystemId &System() const noexcept;
        /** @brief Returns the publication revision. @return Exact non-zero revision. */
        [[nodiscard]] XRCapabilityRevision Revision() const noexcept;
        /** @brief Reads one closed capability state. @param capability Known capability. @return Exact state, or Unsupported for unknown
         * input. */
        [[nodiscard]] XRCapabilityState State(XRCapability capability) const noexcept;
        /** @brief Returns immutable finite capacities. @return Validated limits within XRHardLimits. */
        [[nodiscard]] const XRSystemLimits &Limits() const noexcept;

    private:
        /** @brief Copies validated construction evidence into fixed owned storage. */
        explicit XRCapabilitySnapshot(const XRCapabilityDescriptor &descriptor) noexcept;

        XRContractVersion contractVersion_;
        XRSystemId system_;
        XRCapabilityRevision revision_;
        std::array<XRCapabilityState, static_cast<std::size_t>(XRCapability::Count)> states_;
        XRSystemLimits limits_;
    };

    /**
     * @brief Admits a bounded operation against exact immutable system evidence.
     * @param snapshot Capability evidence captured for the operation.
     * @param activeSystem Exact active system, or invalid after shutdown.
     * @param expectedRevision Revision retained by the caller.
     * @param requirement Capability and finite requested capacities.
     * @return Success or a typed invalid, stale, unsupported, unavailable, incompatible, or capacity result.
     * @post Success performs no native call, allocation, blocking I/O, queue mutation, or CPU-GPU synchronization.
     */
    [[nodiscard]] Result<void> AdmitXRCapability(const XRCapabilitySnapshot &snapshot, const XRSystemId &activeSystem,
                                                 XRCapabilityRevision expectedRevision, const XRCapabilityRequirement &requirement);
}  // namespace Horo::XR
