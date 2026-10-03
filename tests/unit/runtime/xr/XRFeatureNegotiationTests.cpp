#include "Horo/XR/XRFeatureNegotiation.h"
#include "Horo/XR/XRSessionLifecycle.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Horo::XR {
    namespace {
        template <typename T> T Generation(const std::uint64_t value) {
            const auto result = T::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        constexpr std::array ProjectionFeatures{XRCapability::Projection,
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
        constexpr std::array InteractionFeatures{XRCapability::TrackedControllerRoles, XRCapability::ControllerAimGrip,
                                                 XRCapability::SuggestedBindings,      XRCapability::Haptics,
                                                 XRCapability::CancellableHaptics,     XRCapability::RuntimeUiInteraction,
                                                 XRCapability::ComfortPolicy};

        XRCapabilityDescriptor Descriptor(const std::uint64_t runtime = 1, const bool interaction = false) {
            XRCapabilityDescriptor descriptor{
                .system = {.runtime = Generation<XRRuntimeGeneration>(runtime), .slot = {.index = 2, .generation = 1}},
                .revision = Generation<XRCapabilityRevision>(runtime),
                .limits = {.maximumViews = 4, .maximumSpaces = 8, .maximumActions = 16, .maximumDevices = 4},
            };
            descriptor.states.fill(XRCapabilityState::Unsupported);
            for (const XRCapability feature : ProjectionFeatures)
                descriptor.states[static_cast<std::size_t>(feature)] = XRCapabilityState::Available;
            if (interaction) {
                for (const XRCapability feature : InteractionFeatures)
                    descriptor.states[static_cast<std::size_t>(feature)] = XRCapabilityState::Available;
            }
            return descriptor;
        }

        XRCapabilitySnapshot Snapshot(const XRCapabilityDescriptor &descriptor) {
            const auto result = XRCapabilitySnapshot::Create(descriptor);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        XRFeatureNegotiationRequest Request(const XRFeatureProfile profile = XRFeatureProfile::Projection1_0) {
            return {.profile = profile,
                    .requestedLimits = {.maximumViews = 2,
                                        .maximumSpaces = 2,
                                        .maximumActions = 8,
                                        .maximumDevices = profile == XRFeatureProfile::TrackedInteraction1_0 ? 2U : 1U}};
        }

        TEST_CASE("XR feature plan admits exact projection requirements without frame-hot allocation", "[unit][xr][feature]") {
            const auto evidence = Snapshot(Descriptor());
            const auto request = Request();
            const auto before = Horo::Tests::AllocationProbe::Count();
            const auto result = NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request);
            const auto afterNegotiation = Horo::Tests::AllocationProbe::Count();
            REQUIRE(result.status == XRFeatureNegotiationStatus::Ok);
            REQUIRE(result.plan.has_value());
            REQUIRE(afterNegotiation == before);
            const auto beforeUse = Horo::Tests::AllocationProbe::Count();
            const auto current = ValidateXRFeaturePlan(*result.plan, evidence, evidence.System(), evidence.Revision());
            const auto use =
                AdmitXRPlannedFeature(*result.plan, evidence, evidence.System(), evidence.Revision(), XRCapability::Projection);
            const auto afterUse = Horo::Tests::AllocationProbe::Count();
            REQUIRE(current == XRFeatureNegotiationStatus::Ok);
            REQUIRE(use == XRFeatureNegotiationStatus::Ok);
            REQUIRE(afterUse == beforeUse);
            REQUIRE(result.plan->Profile() == XRFeatureProfile::Projection1_0);
            REQUIRE(result.plan->Limits().maximumViews == 2);
            REQUIRE(result.plan->Decision(XRCapability::Projection).state == XRFeatureDecisionState::Required);
            REQUIRE(result.plan->Decision(XRCapability::Haptics).state == XRFeatureDecisionState::NotRequested);
        }

        TEST_CASE("XR negotiation rejects each missing required projection feature with exact evidence", "[unit][xr][feature]") {
            for (const XRCapability feature : ProjectionFeatures) {
                auto descriptor = Descriptor();
                descriptor.states[static_cast<std::size_t>(feature)] = XRCapabilityState::Unsupported;
                const auto evidence = Snapshot(descriptor);
                const auto result = NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), Request());
                REQUIRE(result.status == XRFeatureNegotiationStatus::RequiredUnsupported);
                REQUIRE(result.capability == feature);
                REQUIRE(result.observed == XRCapabilityState::Unsupported);
                REQUIRE_FALSE(result.plan.has_value());
            }
        }

        TEST_CASE("XR tracked interaction never silently downgrades to projection", "[unit][xr][feature]") {
            auto descriptor = Descriptor(1, true);
            const auto supported = Snapshot(descriptor);
            const auto accepted =
                NegotiateXRFeatures(supported, supported.System(), supported.Revision(), Request(XRFeatureProfile::TrackedInteraction1_0));
            REQUIRE(accepted.status == XRFeatureNegotiationStatus::Ok);
            REQUIRE(accepted.plan->Decision(XRCapability::RuntimeUiInteraction).state == XRFeatureDecisionState::Required);
            for (const XRCapability feature : InteractionFeatures) {
                descriptor.states[static_cast<std::size_t>(feature)] = XRCapabilityState::DependencyMissing;
                const auto missing = Snapshot(descriptor);
                const auto rejected =
                    NegotiateXRFeatures(missing, missing.System(), missing.Revision(), Request(XRFeatureProfile::TrackedInteraction1_0));
                REQUIRE(rejected.status == XRFeatureNegotiationStatus::RequiredUnavailable);
                REQUIRE(rejected.capability == feature);
                REQUIRE_FALSE(rejected.plan.has_value());
                descriptor.states[static_cast<std::size_t>(feature)] = XRCapabilityState::Available;
            }
        }

        TEST_CASE("XR required feature failures preserve incompatible and denied categories", "[unit][xr][feature]") {
            for (const auto [state, expected] :
                 {std::pair{XRCapabilityState::Incompatible, XRFeatureNegotiationStatus::RequiredIncompatible},
                  std::pair{XRCapabilityState::Denied, XRFeatureNegotiationStatus::RequiredUnavailable},
                  std::pair{XRCapabilityState::PermissionRequired, XRFeatureNegotiationStatus::RequiredUnavailable}}) {
                auto descriptor = Descriptor();
                descriptor.states[static_cast<std::size_t>(XRCapability::PositionTracking)] = state;
                const auto evidence = Snapshot(descriptor);
                const auto rejected = NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), Request());
                REQUIRE(rejected.status == expected);
                REQUIRE(rejected.capability == XRCapability::PositionTracking);
                REQUIRE(rejected.observed == state);
                REQUIRE_FALSE(rejected.plan.has_value());
            }
        }

        TEST_CASE("XR optional features use only declared disablement or baseline fallback", "[unit][xr][feature]") {
            auto descriptor = Descriptor();
            descriptor.states[static_cast<std::size_t>(XRCapability::DepthComposition)] = XRCapabilityState::DependencyMissing;
            descriptor.states[static_cast<std::size_t>(XRCapability::FixedFoveation)] = XRCapabilityState::Denied;
            const auto evidence = Snapshot(descriptor);
            const std::array extras{XRFeatureRequest{XRCapability::DepthComposition, XRFeatureRequirementKind::Optional,
                                                     XRFeatureFallback::BaselineProjection},
                                    XRFeatureRequest{XRCapability::FixedFoveation, XRFeatureRequirementKind::Optional,
                                                     XRFeatureFallback::Disable}};
            auto request = Request();
            request.features = extras;
            const auto result = NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request);
            REQUIRE(result.status == XRFeatureNegotiationStatus::Ok);
            REQUIRE(result.plan->Decision(XRCapability::DepthComposition).state == XRFeatureDecisionState::BaselineProjection);
            REQUIRE(result.plan->Decision(XRCapability::FixedFoveation).state == XRFeatureDecisionState::OptionalDisabled);
            REQUIRE(result.plan->Decision(XRCapability::FixedFoveation).observed == XRCapabilityState::Denied);
            REQUIRE(AdmitXRPlannedFeature(*result.plan, evidence, evidence.System(), evidence.Revision(), XRCapability::Projection) ==
                    XRFeatureNegotiationStatus::Ok);
            REQUIRE(AdmitXRPlannedFeature(*result.plan, evidence, evidence.System(), evidence.Revision(), XRCapability::DepthComposition) ==
                    XRFeatureNegotiationStatus::FeatureDisabled);
        }

        TEST_CASE("XR optional enabled and explicit disabled states never infer a provider", "[unit][xr][feature]") {
            auto descriptor = Descriptor();
            descriptor.states[static_cast<std::size_t>(XRCapability::DepthComposition)] = XRCapabilityState::Available;
            descriptor.states[static_cast<std::size_t>(XRCapability::RefreshRateSelection)] = XRCapabilityState::Available;
            const auto evidence = Snapshot(descriptor);
            const std::array extras{XRFeatureRequest{XRCapability::DepthComposition, XRFeatureRequirementKind::Optional,
                                                     XRFeatureFallback::Disable},
                                    XRFeatureRequest{XRCapability::RefreshRateSelection, XRFeatureRequirementKind::Disabled,
                                                     XRFeatureFallback::None}};
            auto request = Request();
            request.features = extras;
            const auto result = NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request);
            REQUIRE(result.status == XRFeatureNegotiationStatus::Ok);
            REQUIRE(result.plan->Decision(XRCapability::DepthComposition).state == XRFeatureDecisionState::OptionalEnabled);
            REQUIRE(result.plan->Decision(XRCapability::RefreshRateSelection).state == XRFeatureDecisionState::OptionalDisabled);
            REQUIRE(AdmitXRPlannedFeature(*result.plan, evidence, evidence.System(), evidence.Revision(), XRCapability::DepthComposition) ==
                    XRFeatureNegotiationStatus::Ok);
            REQUIRE(AdmitXRPlannedFeature(*result.plan, evidence, evidence.System(), evidence.Revision(),
                                          XRCapability::RefreshRateSelection) == XRFeatureNegotiationStatus::FeatureDisabled);
        }

        TEST_CASE("XR product-promoted optional capability becomes a hard preflight requirement", "[unit][xr][feature]") {
            const auto evidence = Snapshot(Descriptor());
            const std::array extra{
                XRFeatureRequest{XRCapability::DepthComposition, XRFeatureRequirementKind::Required, XRFeatureFallback::None}};
            auto request = Request();
            request.features = extra;
            const auto rejected = NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request);
            REQUIRE(rejected.status == XRFeatureNegotiationStatus::RequiredUnsupported);
            REQUIRE(rejected.capability == XRCapability::DepthComposition);
            REQUIRE_FALSE(rejected.plan.has_value());
        }

        TEST_CASE("XR malformed, excessive and post-one requests publish no plan", "[unit][xr][feature]") {
            const auto evidence = Snapshot(Descriptor());
            auto request = Request();
            request.requestedLimits.maximumActions = 17;
            REQUIRE(NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request).status ==
                    XRFeatureNegotiationStatus::CapacityExceeded);
            request = Request();
            request.profile = XRFeatureProfile::Count;
            REQUIRE(NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request).status ==
                    XRFeatureNegotiationStatus::InvalidRequest);
            const std::array duplicate{XRFeatureRequest{XRCapability::DepthComposition, XRFeatureRequirementKind::Optional,
                                                        XRFeatureFallback::Disable},
                                       XRFeatureRequest{XRCapability::DepthComposition, XRFeatureRequirementKind::Optional,
                                                        XRFeatureFallback::Disable}};
            request = Request();
            request.features = duplicate;
            REQUIRE(NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request).status ==
                    XRFeatureNegotiationStatus::InvalidRequest);
            const std::array postOne{
                XRFeatureRequest{XRCapability::Passthrough, XRFeatureRequirementKind::Optional, XRFeatureFallback::Disable}};
            request.features = postOne;
            const auto unsupported = NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request);
            REQUIRE(unsupported.status == XRFeatureNegotiationStatus::UnsupportedPath);
            REQUIRE(unsupported.capability == XRCapability::Passthrough);
            REQUIRE_FALSE(unsupported.plan.has_value());
        }

        TEST_CASE("XR optional policy rejects implicit or nonsensical fallback and unbounded inputs", "[unit][xr][feature]") {
            const auto evidence = Snapshot(Descriptor());
            auto request = Request();
            const std::array implicit{XRFeatureRequest{XRCapability::Haptics, XRFeatureRequirementKind::Optional, XRFeatureFallback::None}};
            request.features = implicit;
            REQUIRE(NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request).status ==
                    XRFeatureNegotiationStatus::InvalidRequest);
            const std::array invalidBaseline{
                XRFeatureRequest{XRCapability::Haptics, XRFeatureRequirementKind::Optional, XRFeatureFallback::BaselineProjection}};
            request.features = invalidBaseline;
            REQUIRE(NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request).status ==
                    XRFeatureNegotiationStatus::InvalidRequest);
            const std::array repeatedProfile{
                XRFeatureRequest{XRCapability::Projection, XRFeatureRequirementKind::Required, XRFeatureFallback::None}};
            request.features = repeatedProfile;
            REQUIRE(NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request).status ==
                    XRFeatureNegotiationStatus::InvalidRequest);
            std::array<XRFeatureRequest, static_cast<std::size_t>(XRCapability::Count) + 1> oversized{};
            request.features = oversized;
            REQUIRE(NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), request).status ==
                    XRFeatureNegotiationStatus::InvalidRequest);
        }

        TEST_CASE("XR plan is fenced by system, revision, evidence and shutdown", "[unit][xr][feature]") {
            const auto evidence = Snapshot(Descriptor());
            const auto resolved = NegotiateXRFeatures(evidence, evidence.System(), evidence.Revision(), Request());
            REQUIRE(resolved.plan.has_value());
            const auto replacement = Snapshot(Descriptor(2));
            REQUIRE(ValidateXRFeaturePlan(*resolved.plan, replacement, replacement.System(), replacement.Revision()) ==
                    XRFeatureNegotiationStatus::StaleSystem);
            REQUIRE(AdmitXRPlannedFeature(*resolved.plan, replacement, replacement.System(), replacement.Revision(),
                                          XRCapability::Projection) == XRFeatureNegotiationStatus::StaleSystem);
            REQUIRE(ValidateXRFeaturePlan(*resolved.plan, evidence, {}, evidence.Revision()) == XRFeatureNegotiationStatus::Unavailable);
            REQUIRE(ValidateXRFeaturePlan(*resolved.plan, evidence, evidence.System(), replacement.Revision()) ==
                    XRFeatureNegotiationStatus::StaleRevision);
            auto changed = Descriptor();
            changed.states[static_cast<std::size_t>(XRCapability::Projection)] = XRCapabilityState::Lost;
            REQUIRE(ValidateXRFeaturePlan(*resolved.plan, Snapshot(changed), evidence.System(), evidence.Revision()) ==
                    XRFeatureNegotiationStatus::StaleRevision);
            REQUIRE(AdmitXRPlannedFeature(*resolved.plan, evidence, evidence.System(), evidence.Revision(), XRCapability::Count) ==
                    XRFeatureNegotiationStatus::InvalidRequest);
        }
    }  // namespace
}  // namespace Horo::XR
