#pragma once

/**
 * @file XRFeatureNegotiation.h
 * @brief Bounded feature-policy resolution before XR session resource preparation.
 */

#include "Horo/XR/XRCapabilities.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Horo::XR {
    /** @brief Version-one product requirement sets, not ordered quality tiers. */
    enum class XRFeatureProfile : std::uint8_t {
        Projection1_0,
        TrackedInteraction1_0,
        Count
    };

    /** @brief Product intent for one extra feature outside the fixed profile requirements. */
    enum class XRFeatureRequirementKind : std::uint8_t {
        Required,
        Optional,
        Disabled,
        Count
    };

    /** @brief Explicit behavior when an optional feature lacks admissible evidence. */
    enum class XRFeatureFallback : std::uint8_t {
        None,
        Disable,
        BaselineProjection,
        Count
    };

    /** @brief One additional product policy entry; the fixed profile requirements need no duplicate entries. */
    struct XRFeatureRequest final {
        XRCapability capability{XRCapability::Count};                      /**< Known feature, never a native extension name. */
        XRFeatureRequirementKind kind{XRFeatureRequirementKind::Required}; /**< Required, optional, or explicitly disabled. */
        XRFeatureFallback fallback{XRFeatureFallback::None};               /**< Optional missing-feature policy; None otherwise. */
    };

    /** @brief Complete, borrowed preflight request; the resolver copies decisions and retains no span. */
    struct XRFeatureNegotiationRequest final {
        XRFeatureProfile profile{XRFeatureProfile::Count}; /**< Exact 1.0 profile required by the product. */
        XRSystemLimits requestedLimits{};                  /**< Finite session budget; primary stereo requests exactly two views. */
        std::span<const XRFeatureRequest> features{};      /**< Additional closed, unique product feature policies. */
    };

    /** @brief One resolved feature disposition, including the observed non-boolean evidence. */
    enum class XRFeatureDecisionState : std::uint8_t {
        NotRequested,
        Required,
        OptionalEnabled,
        OptionalDisabled,
        BaselineProjection
    };

    /** @brief Immutable plan entry returned by value, never a native provider or extension handle. */
    struct XRFeatureDecision final {
        XRFeatureDecisionState state{XRFeatureDecisionState::NotRequested}; /**< Admitted or explicitly disabled/fallback. */
        XRCapabilityState observed{XRCapabilityState::Unsupported};         /**< Exact discovery state behind the decision. */
    };

    /** @brief Typed preflight result; no plan is published on any failure. */
    enum class XRFeatureNegotiationStatus : std::uint8_t {
        Ok,
        InvalidRequest,
        Unavailable,
        UnsupportedPath,
        StaleSystem,
        StaleRevision,
        CapacityExceeded,
        RequiredUnsupported,
        RequiredIncompatible,
        RequiredUnavailable,
        FeatureDisabled
    };

    struct XRFeatureNegotiationOutcome;

    /**
     * @brief Immutable, fixed-size decision publication for one exact system and capability revision.
     *
     * Only NegotiateXRFeatures can construct a plan. A retained plan is memory-valid after owner replacement, but
     * ValidateXRFeaturePlan must recheck currentness immediately before resource preparation or feature use.
     */
    class XRFeaturePlan final {
    public:
        /** @brief Returns the admitted product profile. @return Exact version-one profile. */
        [[nodiscard]] XRFeatureProfile Profile() const noexcept;
        /** @brief Returns the source system generation. @return Exact owner identity. */
        [[nodiscard]] const XRSystemId &System() const noexcept;
        /** @brief Returns the source capability revision. @return Exact immutable evidence revision. */
        [[nodiscard]] XRCapabilityRevision Revision() const noexcept;
        /** @brief Returns the admitted finite resource budget. @return Copied product limit request. */
        [[nodiscard]] const XRSystemLimits &Limits() const noexcept;
        /** @brief Reads one resolved feature. @param capability Closed feature ID. @return Decision or NotRequested for unknown IDs. */
        [[nodiscard]] XRFeatureDecision Decision(XRCapability capability) const noexcept;

    private:
        friend XRFeatureNegotiationOutcome NegotiateXRFeatures(const XRCapabilitySnapshot &, const XRSystemId &, XRCapabilityRevision,
                                                               const XRFeatureNegotiationRequest &) noexcept;

        /** @brief Copies a completely resolved publication after every profile and policy decision passes preflight. */
        XRFeaturePlan(XRFeatureProfile profile, const XRCapabilitySnapshot &evidence, XRSystemLimits limits,
                      std::array<XRFeatureDecision, static_cast<std::size_t>(XRCapability::Count)> decisions) noexcept;

        XRFeatureProfile profile_;
        XRSystemId system_;
        XRCapabilityRevision revision_;
        XRSystemLimits limits_;
        std::array<XRFeatureDecision, static_cast<std::size_t>(XRCapability::Count)> decisions_;
    };

    /** @brief Typed status and failed feature/state, with an all-or-nothing immutable plan. */
    struct XRFeatureNegotiationOutcome final {
        XRFeatureNegotiationStatus status{XRFeatureNegotiationStatus::InvalidRequest}; /**< Preflight category. */
        XRCapability capability{XRCapability::Count};               /**< First failed feature, or Count for owner/shape failures. */
        XRCapabilityState observed{XRCapabilityState::Unsupported}; /**< Evidence behind feature failure. */
        std::optional<XRFeaturePlan> plan;                          /**< Present only when status is Ok. */
    };

    /**
     * @brief Resolve a complete product profile and extra policies against one immutable capability publication.
     * @param evidence Exact selected-system evidence, copied into decisions rather than retained by reference.
     * @param activeSystem Current host-selected system; invalid after shutdown.
     * @param expectedRevision Host-retained capability revision.
     * @param request Closed profile, finite budget, and unique explicit extra-feature policies.
     * @return Complete plan or typed owner, capacity, malformed, required-feature, or unsupported-path failure.
     * @post Failure creates no plan or resource; this bounded preflight performs no native call or heap allocation.
     */
    [[nodiscard]] XRFeatureNegotiationOutcome NegotiateXRFeatures(const XRCapabilitySnapshot &evidence, const XRSystemId &activeSystem,
                                                                  XRCapabilityRevision expectedRevision,
                                                                  const XRFeatureNegotiationRequest &request) noexcept;

    /**
     * @brief Revalidate a retained accepted plan before session preparation or feature dispatch.
     * @param plan Previously negotiated immutable plan.
     * @param evidence Current immutable capability publication.
     * @param activeSystem Current host-selected system, invalid after shutdown.
     * @param expectedRevision Current host-retained capability revision.
     * @return Ok or typed stale/invalid status without native work or mutation.
     */
    [[nodiscard]] XRFeatureNegotiationStatus ValidateXRFeaturePlan(const XRFeaturePlan &plan, const XRCapabilitySnapshot &evidence,
                                                                   const XRSystemId &activeSystem,
                                                                   XRCapabilityRevision expectedRevision) noexcept;

    /**
     * @brief Fail closed before a planned feature reaches its adapter or frame-hot consumer.
     * @param plan Immutable admitted plan.
     * @param evidence Current capability publication.
     * @param activeSystem Current selected owner, invalid after shutdown.
     * @param expectedRevision Current host-retained publication revision.
     * @param capability Feature about to execute, never a native extension name.
     * @return Ok only for an exact current enabled feature; disabled, unrequested, stale, or invalid paths fail.
     * @post Fixed array lookup with no allocation, native call, blocking I/O, or CPU/GPU synchronization.
     */
    [[nodiscard]] XRFeatureNegotiationStatus AdmitXRPlannedFeature(const XRFeaturePlan &plan, const XRCapabilitySnapshot &evidence,
                                                                   const XRSystemId &activeSystem, XRCapabilityRevision expectedRevision,
                                                                   XRCapability capability) noexcept;
}  // namespace Horo::XR
