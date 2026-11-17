#pragma once
#include "Horo/XR/XRFakeRuntime.h"
#include "XRHandTestSupport.h"

namespace Horo::XR::SimulatorTests {
    inline XRCapabilitySnapshot Capabilities() {
        XRCapabilityDescriptor descriptor{.system = {Value<XRRuntimeGeneration>(1), {1, 1}},
                                          .revision = Value<XRCapabilityRevision>(1),
                                          .limits = {2, 8, 8, 8}};
        descriptor.states.fill(XRCapabilityState::Available);
        return XRCapabilitySnapshot::Create(descriptor).Value();
    }

    inline XRFeaturePlan Plan(const XRCapabilitySnapshot &capabilities, const bool hands = true) {
        const std::array requests{XRFeatureRequest{XRCapability::HandTracking, XRFeatureRequirementKind::Required}};
        const auto plan =
            NegotiateXRFeatures(capabilities, capabilities.System(), capabilities.Revision(),
                                {.profile = XRFeatureProfile::Projection1_0,
                                 .requestedLimits = {2, 8, 8, 5},
                                 .features = hands ? std::span<const XRFeatureRequest>{requests} : std::span<const XRFeatureRequest>{}});
        REQUIRE(plan.status == XRFeatureNegotiationStatus::Ok);
        return *plan.plan;
    }

    inline XRFakeStep Event(const XRSessionEvent event) {
        return {.event = event, .executeFrame = false, .shouldRender = false};
    }

    inline XRFakeStep Frame(const XRSessionId &session, const std::int64_t time = 100) {
        XRFakeStep step;
        step.prediction = Value<XRRenderPredictionTime>(time + 1);
        step.viewCount = 2;
        auto descriptor = Pose(session, time);
        descriptor.source.kind = XRSpaceKind::View;
        descriptor.purpose = XRPosePurpose::PresentationPrediction;
        descriptor.time.simulation.reset();
        descriptor.time.renderPrediction = step.prediction;
        const auto sample = XRPoseSample::Create(descriptor, session, Origin());
        REQUIRE(sample.HasValue());
        step.views[0] = sample.Value();
        step.views[1] = sample.Value();
        step.actionSampleTime = Value<XRRuntimeTime>(time);
        return step;
    }

}  // namespace Horo::XR::SimulatorTests
