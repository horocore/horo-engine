#include "Horo/XR/XRFeatureNegotiation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace Horo::XR {
    namespace {
        constexpr std::size_t FeatureCount = static_cast<std::size_t>(XRCapability::Count);
        using Decisions = std::array<XRFeatureDecision, FeatureCount>;

        constexpr std::array ProjectionRequired{XRCapability::Projection,
                                                XRCapability::PrimaryOpaqueStereo,
                                                XRCapability::OrientationTracking,
                                                XRCapability::PositionTracking,
                                                XRCapability::ViewSpace,
                                                XRCapability::LocalSpace,
                                                XRCapability::BooleanActions,
                                                XRCapability::FloatActions,
                                                XRCapability::Vector2Actions,
                                                XRCapability::PoseActions,
                                                XRCapability::PredictedFrames,
                                                XRCapability::ExternalColorTargets,
                                                XRCapability::SessionLossLifecycle,
                                                XRCapability::CanonicalInputProjection};
        constexpr std::array InteractionRequired{XRCapability::TrackedControllerRoles, XRCapability::ControllerAimGrip,
                                                 XRCapability::SuggestedBindings,      XRCapability::Haptics,
                                                 XRCapability::CancellableHaptics,     XRCapability::RuntimeUiInteraction,
                                                 XRCapability::ComfortPolicy};

        [[nodiscard]] constexpr bool IsKnown(const XRCapability capability) noexcept {
            return capability < XRCapability::Count;
        }

        [[nodiscard]] bool IsProfileRequired(const XRFeatureProfile profile, const XRCapability capability) noexcept {
            const auto contains = [capability](const auto &values) {
                return std::ranges::find(values, capability) != values.end();
            };
            return contains(ProjectionRequired) || (profile == XRFeatureProfile::TrackedInteraction1_0 && contains(InteractionRequired));
        }

        [[nodiscard]] constexpr bool IsPostOne(const XRCapability capability) noexcept {
            using enum XRCapability;
            return capability == Passthrough || capability == EyeGaze || capability == QuadViews || capability == SpaceWarp;
        }

        [[nodiscard]] constexpr bool AllowsProjectionFallback(const XRCapability capability) noexcept {
            using enum XRCapability;
            return capability == DepthComposition || capability == FixedFoveation || capability == RefreshRateSelection ||
                   capability == VisibilityMask;
        }

        [[nodiscard]] constexpr bool IsOptionalOne(const XRFeatureProfile profile, const XRCapability capability) noexcept {
            using enum XRCapability;
            return capability == StageSpace || capability == DepthComposition || capability == FixedFoveation ||
                   capability == RefreshRateSelection || capability == VisibilityMask || capability == AndroidStandalone ||
                   (profile == XRFeatureProfile::TrackedInteraction1_0 && capability == ControllerPresentation);
        }

        [[nodiscard]] bool LimitsFit(const XRSystemLimits &requested, const XRSystemLimits &available) noexcept {
            return requested.maximumViews <= available.maximumViews && requested.maximumSpaces <= available.maximumSpaces &&
                   requested.maximumActions <= available.maximumActions && requested.maximumDevices <= available.maximumDevices;
        }

        [[nodiscard]] bool ValidLimits(const XRFeatureNegotiationRequest &request) noexcept {
            const XRSystemLimits &limits = request.requestedLimits;
            return limits.maximumViews == 2 && limits.maximumSpaces > 0 && limits.maximumActions > 0 &&
                   limits.maximumDevices >= (request.profile == XRFeatureProfile::TrackedInteraction1_0 ? 2U : 1U);
        }

        [[nodiscard]] bool ValidExtraRequest(const XRFeatureProfile profile, const XRFeatureRequest &feature) noexcept {
            if (!IsKnown(feature.capability) || IsPostOne(feature.capability) || !IsOptionalOne(profile, feature.capability) ||
                IsProfileRequired(profile, feature.capability) || feature.kind >= XRFeatureRequirementKind::Count ||
                feature.fallback >= XRFeatureFallback::Count)
                return false;
            if (feature.kind == XRFeatureRequirementKind::Optional)
                return feature.fallback == XRFeatureFallback::Disable ||
                       (feature.fallback == XRFeatureFallback::BaselineProjection && AllowsProjectionFallback(feature.capability));
            return feature.fallback == XRFeatureFallback::None;
        }

        [[nodiscard]] bool ValidRequestShape(const XRFeatureNegotiationRequest &request) noexcept {
            if (request.profile >= XRFeatureProfile::Count || !ValidLimits(request) || request.features.size() > FeatureCount)
                return false;
            std::array<bool, FeatureCount> seen{};
            for (const XRFeatureRequest &feature : request.features) {
                if (!ValidExtraRequest(request.profile, feature) || seen[static_cast<std::size_t>(feature.capability)])
                    return false;
                seen[static_cast<std::size_t>(feature.capability)] = true;
            }
            return true;
        }

        [[nodiscard]] constexpr XRFeatureNegotiationStatus RequiredFailure(const XRCapabilityState state) noexcept {
            using enum XRFeatureNegotiationStatus;
            if (state == XRCapabilityState::Unsupported)
                return RequiredUnsupported;
            if (state == XRCapabilityState::Incompatible)
                return RequiredIncompatible;
            return RequiredUnavailable;
        }

        [[nodiscard]] XRFeatureNegotiationOutcome ResolveRequired(const XRCapabilitySnapshot &evidence,
                                                                  const std::span<const XRCapability> required,
                                                                  Decisions &decisions) noexcept {
            for (const XRCapability capability : required) {
                const XRCapabilityState state = evidence.State(capability);
                if (state != XRCapabilityState::Available)
                    return {RequiredFailure(state), capability, state, std::nullopt};
                decisions[static_cast<std::size_t>(capability)] = {XRFeatureDecisionState::Required, state};
            }
            return {XRFeatureNegotiationStatus::Ok, XRCapability::Count, XRCapabilityState::Unsupported, std::nullopt};
        }

        [[nodiscard]] XRFeatureNegotiationOutcome ResolveExtras(const XRCapabilitySnapshot &evidence,
                                                                const std::span<const XRFeatureRequest> extras,
                                                                Decisions &decisions) noexcept {
            for (const XRFeatureRequest &feature : extras) {
                const XRCapabilityState state = evidence.State(feature.capability);
                XRFeatureDecisionState decision = XRFeatureDecisionState::NotRequested;
                if (feature.kind == XRFeatureRequirementKind::Required) {
                    if (state != XRCapabilityState::Available)
                        return {RequiredFailure(state), feature.capability, state, std::nullopt};
                    decision = XRFeatureDecisionState::Required;
                } else if (feature.kind == XRFeatureRequirementKind::Disabled) {
                    decision = XRFeatureDecisionState::OptionalDisabled;
                } else if (state == XRCapabilityState::Available) {
                    decision = XRFeatureDecisionState::OptionalEnabled;
                } else {
                    decision = feature.fallback == XRFeatureFallback::Disable ? XRFeatureDecisionState::OptionalDisabled
                                                                              : XRFeatureDecisionState::BaselineProjection;
                }
                decisions[static_cast<std::size_t>(feature.capability)] = {decision, state};
            }
            return {XRFeatureNegotiationStatus::Ok, XRCapability::Count, XRCapabilityState::Unsupported, std::nullopt};
        }
    }  // namespace

    /** @copydoc XRFeaturePlan::XRFeaturePlan */
    XRFeaturePlan::XRFeaturePlan(const XRFeatureProfile profile, const XRCapabilitySnapshot &evidence, const XRSystemLimits limits,
                                 Decisions decisions) noexcept
        : profile_(profile), system_(evidence.System()), revision_(evidence.Revision()), limits_(limits), decisions_(std::move(decisions)) {
    }

    /** @copydoc XRFeaturePlan::Profile */
    XRFeatureProfile XRFeaturePlan::Profile() const noexcept {
        return profile_;
    }

    /** @copydoc XRFeaturePlan::System */
    const XRSystemId &XRFeaturePlan::System() const noexcept {
        return system_;
    }

    /** @copydoc XRFeaturePlan::Revision */
    XRCapabilityRevision XRFeaturePlan::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc XRFeaturePlan::Limits */
    const XRSystemLimits &XRFeaturePlan::Limits() const noexcept {
        return limits_;
    }

    /** @copydoc XRFeaturePlan::Decision */
    XRFeatureDecision XRFeaturePlan::Decision(const XRCapability capability) const noexcept {
        if (!IsKnown(capability))
            return {};
        return decisions_[static_cast<std::size_t>(capability)];
    }

    /** @copydoc NegotiateXRFeatures */
    XRFeatureNegotiationOutcome NegotiateXRFeatures(const XRCapabilitySnapshot &evidence, const XRSystemId &activeSystem,
                                                    const XRCapabilityRevision expectedRevision,
                                                    const XRFeatureNegotiationRequest &request) noexcept {
        using enum XRFeatureNegotiationStatus;
        if (!activeSystem.IsValid())
            return {Unavailable, XRCapability::Count, {}, std::nullopt};
        if (!expectedRevision.IsValid() || request.profile >= XRFeatureProfile::Count || request.features.size() > FeatureCount)
            return {InvalidRequest, XRCapability::Count, {}, std::nullopt};
        if (evidence.System() != activeSystem)
            return {StaleSystem, XRCapability::Count, {}, std::nullopt};
        if (evidence.Revision() != expectedRevision)
            return {StaleRevision, XRCapability::Count, {}, std::nullopt};
        for (const XRFeatureRequest &feature : request.features) {
            if (IsPostOne(feature.capability))
                return {UnsupportedPath, feature.capability, evidence.State(feature.capability), std::nullopt};
        }
        if (!ValidRequestShape(request))
            return {InvalidRequest, XRCapability::Count, {}, std::nullopt};
        if (!LimitsFit(request.requestedLimits, evidence.Limits()))
            return {CapacityExceeded, XRCapability::Count, {}, std::nullopt};

        Decisions decisions{};
        if (const auto projection = ResolveRequired(evidence, ProjectionRequired, decisions); projection.status != Ok)
            return projection;
        if (request.profile == XRFeatureProfile::TrackedInteraction1_0) {
            if (const auto interaction = ResolveRequired(evidence, InteractionRequired, decisions); interaction.status != Ok)
                return interaction;
        }
        if (const auto extras = ResolveExtras(evidence, request.features, decisions); extras.status != Ok)
            return extras;
        XRFeaturePlan plan{request.profile, evidence, request.requestedLimits, std::move(decisions)};
        return {Ok, XRCapability::Count, {}, std::move(plan)};
    }

    /** @copydoc ValidateXRFeaturePlan */
    XRFeatureNegotiationStatus ValidateXRFeaturePlan(const XRFeaturePlan &plan, const XRCapabilitySnapshot &evidence,
                                                     const XRSystemId &activeSystem, const XRCapabilityRevision expectedRevision) noexcept {
        using enum XRFeatureNegotiationStatus;
        if (!activeSystem.IsValid())
            return Unavailable;
        if (!expectedRevision.IsValid())
            return InvalidRequest;
        if (plan.System() != activeSystem || evidence.System() != activeSystem)
            return StaleSystem;
        if (plan.Revision() != expectedRevision || evidence.Revision() != expectedRevision)
            return StaleRevision;
        if (!LimitsFit(plan.Limits(), evidence.Limits()))
            return CapacityExceeded;
        for (std::size_t index = 0; index < FeatureCount; ++index) {
            const auto capability = static_cast<XRCapability>(index);
            const XRFeatureDecision decision = plan.Decision(capability);
            if (decision.state != XRFeatureDecisionState::NotRequested && decision.observed != evidence.State(capability))
                return StaleRevision;
        }
        return Ok;
    }

    /** @copydoc AdmitXRPlannedFeature */
    XRFeatureNegotiationStatus AdmitXRPlannedFeature(const XRFeaturePlan &plan, const XRCapabilitySnapshot &evidence,
                                                     const XRSystemId &activeSystem, const XRCapabilityRevision expectedRevision,
                                                     const XRCapability capability) noexcept {
        using enum XRFeatureNegotiationStatus;
        if (!IsKnown(capability))
            return InvalidRequest;
        if (const auto current = ValidateXRFeaturePlan(plan, evidence, activeSystem, expectedRevision); current != Ok)
            return current;
        if (const XRFeatureDecisionState decision = plan.Decision(capability).state;
            decision == XRFeatureDecisionState::Required || decision == XRFeatureDecisionState::OptionalEnabled)
            return Ok;
        return FeatureDisabled;
    }
}  // namespace Horo::XR
