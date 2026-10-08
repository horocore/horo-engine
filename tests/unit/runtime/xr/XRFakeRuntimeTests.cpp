#include "Horo/XR/XRFakeRuntime.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::XR {
    namespace {
        template <typename T> T Generation(const std::uint64_t value) {
            auto created = T::Create(value);
            REQUIRE(created.HasValue());
            return created.Value();
        }

        XRCapabilitySnapshot Capabilities(const std::uint64_t runtime = 1) {
            XRCapabilityDescriptor descriptor;
            descriptor.system = {.runtime = Generation<XRRuntimeGeneration>(runtime), .slot = {.index = 2, .generation = 1}};
            descriptor.revision = Generation<XRCapabilityRevision>(runtime);
            descriptor.limits = {.maximumViews = 4, .maximumSpaces = 8, .maximumActions = 8, .maximumDevices = 4};
            descriptor.states.fill(XRCapabilityState::Available);
            const auto snapshot = XRCapabilitySnapshot::Create(descriptor);
            REQUIRE(snapshot.HasValue());
            return snapshot.Value();
        }

        XRFeaturePlan Plan(const XRCapabilitySnapshot &capabilities) {
            const auto result =
                NegotiateXRFeatures(capabilities, capabilities.System(), capabilities.Revision(),
                                    {.profile = XRFeatureProfile::Projection1_0,
                                     .requestedLimits = {.maximumViews = 2, .maximumSpaces = 2, .maximumActions = 4, .maximumDevices = 1}});
            REQUIRE(result.status == XRFeatureNegotiationStatus::Ok);
            return *result.plan;
        }

        XRSessionId Activate(XRFakeRuntime &runtime, const std::uint64_t generation = 1) {
            const auto capabilities = Capabilities(generation);
            auto result = runtime.Activate(capabilities, Plan(capabilities), {.maximumViews = 2, .maximumImages = 1, .maximumLayers = 1});
            REQUIRE(result.HasValue());
            return result.Value();
        }

        XRFakeStep Event(const XRSessionEvent event) {
            return {.event = event, .executeFrame = false, .shouldRender = false};
        }

        XRFakeStep Frame(const XRSessionId &session, const std::int64_t time = 100, const bool lost = false) {
            XRFakeStep step;
            step.prediction = XRRenderPredictionTime::Create(time).Value();
            step.viewCount = 2;
            const auto origin = Generation<XRWorldOriginRevision>(1);
            for (std::uint32_t i = 0; i < 2; ++i) {
                XRPoseDescriptor descriptor{.session = session,
                                            .source = {.id = {session, {.index = i + 1, .generation = 1}},
                                                       .kind = XRSpaceKind::View,
                                                       .worldOriginRevision = origin},
                                            .target = {.id = {session, {.index = 3, .generation = 1}},
                                                       .kind = XRSpaceKind::Local,
                                                       .worldOriginRevision = origin},
                                            .purpose = XRPosePurpose::PresentationPrediction,
                                            .time = {.runtimeSample = XRRuntimeTime::Create(time).Value(),
                                                     .renderPrediction = step.prediction}};
                if (!lost) {
                    descriptor.components.positionMeters = {.value = Math::Vec3{}, .validity = XRPoseComponentValidity::Tracked};
                    descriptor.components.orientation = {.value = Math::Quaternion{}, .validity = XRPoseComponentValidity::Tracked};
                    descriptor.components.confidence = XRTrackingConfidence::High;
                    descriptor.components.loss = XRTrackingLossState::None;
                }
                const auto pose = XRPoseSample::Create(descriptor, session, origin);
                REQUIRE(pose.HasValue());
                step.views[i] = pose.Value();
            }
            step.actionSampleTime = XRRuntimeTime::Create(time - 1).Value();
            step.actionCount = 1;
            step.actions[0] = {.action = {session, {.index = 1, .generation = 1}}, .value = {1, 0}, .active = true};
            return step;
        }

        TEST_CASE("Fake XR owns scripts and publishes deterministic focused frames without allocation", "[unit][xr][fake]") {
            XRFakeRuntime runtime;
            const auto session = Activate(runtime);
            std::array script{Event(XRSessionEvent::Started), Event(XRSessionEvent::BecameVisible), Event(XRSessionEvent::BecameFocused),
                              Frame(session)};
            REQUIRE(runtime.LoadScript(session, script) == XRFrameStatus::Ok);
            script[3].actionCount = 0;
            for (int i = 0; i < 3; ++i)
                REQUIRE(runtime.Step(session).status == XRFrameStatus::Ok);
            const auto before = Horo::Tests::AllocationProbe::Count();
            const auto outcome = runtime.Step(session);
            const auto after = Horo::Tests::AllocationProbe::Count();
            REQUIRE(before == after);
            REQUIRE(outcome.status == XRFrameStatus::Ok);
            REQUIRE(outcome.consumed);
            REQUIRE(outcome.nextStep == 4);
            REQUIRE(outcome.frame.predictedDisplayTime.Nanoseconds() == 100);
            REQUIRE(outcome.frame.acquiredImages == 1);
            REQUIRE(outcome.frame.releasedImages == 1);
            REQUIRE(outcome.viewCount == 2);
            REQUIRE(outcome.actions[0].active);
            REQUIRE(runtime.Step(session).status == XRFrameStatus::Unavailable);
        }

        TEST_CASE("Fake XR tracking and focus loss neutralize actions and preserve explicit pose validity", "[unit][xr][fake]") {
            XRFakeRuntime runtime;
            const auto session = Activate(runtime);
            const std::array script{Event(XRSessionEvent::Started),       Event(XRSessionEvent::BecameVisible),
                                    Event(XRSessionEvent::BecameFocused), Frame(session, 100, true),
                                    Event(XRSessionEvent::FocusLost),     Frame(session, 200)};
            REQUIRE(runtime.LoadScript(session, script) == XRFrameStatus::Ok);
            for (int i = 0; i < 3; ++i)
                REQUIRE(runtime.Step(session).status == XRFrameStatus::Ok);
            auto outcome = runtime.Step(session);
            REQUIRE(outcome.status == XRFrameStatus::Ok);
            REQUIRE_FALSE(outcome.actions[0].active);
            REQUIRE(outcome.actions[0].value[0] == 0);
            REQUIRE(outcome.views[0]->Components().loss == XRTrackingLossState::FullyLost);
            REQUIRE(runtime.Step(session).status == XRFrameStatus::Ok);
            outcome = runtime.Step(session);
            REQUIRE(outcome.status == XRFrameStatus::Ok);
            REQUIRE_FALSE(outcome.actions[0].active);
        }

        TEST_CASE("Fake XR failure sequences close all frame debt and admit subsequent work", "[unit][xr][fake]") {
            for (const auto failure : {XRFakeFailure::Wait, XRFakeFailure::Begin, XRFakeFailure::Locate, XRFakeFailure::Acquire,
                                       XRFakeFailure::Submit, XRFakeFailure::Release, XRFakeFailure::End}) {
                XRFakeRuntime runtime;
                const auto session = Activate(runtime);
                auto failed = Frame(session);
                failed.failure = failure;
                const std::array script{Event(XRSessionEvent::Started), failed, Frame(session, 200)};
                REQUIRE(runtime.LoadScript(session, script) == XRFrameStatus::Ok);
                REQUIRE(runtime.Step(session).status == XRFrameStatus::Ok);
                const auto outcome = runtime.Step(session);
                REQUIRE(outcome.status == XRFrameStatus::Unavailable);
                REQUIRE(outcome.consumed);
                REQUIRE(outcome.failure == failure);
                REQUIRE(outcome.actionCount == 0);
                if (failure == XRFakeFailure::Submit)
                    REQUIRE(outcome.frame.phase == XRFramePhase::ImagesAcquired);
                if (failure == XRFakeFailure::Release)
                    REQUIRE(outcome.frame.phase == XRFramePhase::RendererSubmitted);
                if (failure == XRFakeFailure::End)
                    REQUIRE(outcome.frame.phase == XRFramePhase::ImagesReleased);
                REQUIRE(runtime.Step(session).status == XRFrameStatus::Ok);
            }
        }

        TEST_CASE("Fake XR invalid script replacement and activation leave the prior publication intact", "[unit][xr][fake]") {
            XRFakeRuntime runtime;
            const auto session = Activate(runtime);
            const std::array script{Event(XRSessionEvent::Started), Frame(session)};
            REQUIRE(runtime.LoadScript(session, script) == XRFrameStatus::Ok);
            auto invalid = Frame(session);
            invalid.actionCount = 65;
            REQUIRE(runtime.LoadScript(session, std::span{&invalid, 1}) == XRFrameStatus::CapacityExceeded);
            invalid = Frame(session);
            invalid.actions[0].value[0] = std::numeric_limits<float>::quiet_NaN();
            REQUIRE(runtime.LoadScript(session, std::span{&invalid, 1}) == XRFrameStatus::InvalidInput);
            invalid = Frame(session);
            invalid.actions[0].kind = XRFakeActionKind::Count;
            REQUIRE(runtime.LoadScript(session, std::span{&invalid, 1}) == XRFrameStatus::InvalidInput);
            const auto replacement = Capabilities(2);
            for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(XRSessionPreparation::Count); ++i) {
                REQUIRE(runtime
                            .Activate(replacement, Plan(replacement), {.maximumViews = 2, .maximumImages = 1, .maximumLayers = 1},
                                      static_cast<XRSessionPreparation>(i))
                            .HasError());
                REQUIRE(runtime.Snapshot().session == session);
            }
            REQUIRE(runtime.Step(session).status == XRFrameStatus::Ok);
            REQUIRE(runtime.Step(session).status == XRFrameStatus::Ok);
        }

        TEST_CASE("Fake XR script boundaries reject unsupported stale and malformed evidence", "[unit][xr][fake]") {
            XRFakeRuntime runtime;
            const auto session = Activate(runtime);
            auto step = Frame(session);
            step.shouldRender = false;
            step.failure = XRFakeFailure::Acquire;
            REQUIRE(runtime.LoadScript(session, std::span{&step, 1}) == XRFrameStatus::Unsupported);
            step = Frame(session);
            step.event = static_cast<XRSessionEvent>(255);
            REQUIRE(runtime.LoadScript(session, std::span{&step, 1}) == XRFrameStatus::InvalidInput);
            step = Frame(session);
            step.actionCount = 2;
            step.actions[1] = step.actions[0];
            REQUIRE(runtime.LoadScript(session, std::span{&step, 1}) == XRFrameStatus::Duplicate);
            step = Frame(session);
            step.prediction = {};
            REQUIRE(runtime.LoadScript(session, std::span{&step, 1}) == XRFrameStatus::InvalidInput);
            const std::array unordered{Frame(session, 200), Frame(session, 100)};
            REQUIRE(runtime.LoadScript(session, unordered) == XRFrameStatus::InvalidInput);
            std::vector<XRFakeStep> oversized(XRFakeLimits::MaximumSteps + 1);
            REQUIRE(runtime.LoadScript(session, oversized) == XRFrameStatus::CapacityExceeded);
            oversized.resize(XRFakeLimits::MaximumSteps);
            std::fill(oversized.begin(), oversized.end(), Event(XRSessionEvent::Started));
            REQUIRE(runtime.LoadScript(session, oversized) == XRFrameStatus::Ok);
            REQUIRE(runtime.Step({}).status == XRFrameStatus::InvalidInput);
            const std::array illegal{Event(XRSessionEvent::BecameFocused)};
            REQUIRE(runtime.LoadScript(session, illegal) == XRFrameStatus::Ok);
            const auto rejected = runtime.Step(session);
            REQUIRE(rejected.status == XRFrameStatus::OutOfOrder);
            REQUIRE_FALSE(rejected.consumed);
            REQUIRE(rejected.nextStep == 0);
            REQUIRE(runtime.Snapshot().state == XRSessionState::Ready);
            const std::array notRunning{Frame(session)};
            REQUIRE(runtime.LoadScript(session, notRunning) == XRFrameStatus::Ok);
            const auto unavailable = runtime.Step(session);
            REQUIRE(unavailable.status == XRFrameStatus::Unavailable);
            REQUIRE(unavailable.consumed);
        }

        TEST_CASE("Fake XR runtime restart fences old scripts and shutdown permanently closes admission", "[unit][xr][fake]") {
            XRFakeRuntime runtime;
            const auto first = Activate(runtime);
            const std::array script{Event(XRSessionEvent::Started), Frame(first)};
            REQUIRE(runtime.LoadScript(first, script) == XRFrameStatus::Ok);
            REQUIRE(runtime.Step(first).status == XRFrameStatus::Ok);
            const auto old = runtime.Step(first);
            const std::array loss{Event(XRSessionEvent::InstanceLost)};
            REQUIRE(runtime.LoadScript(first, loss) == XRFrameStatus::Ok);
            REQUIRE(runtime.Step(first).session.state == XRSessionState::Lost);
            const auto second = Activate(runtime, 2);
            REQUIRE(runtime.Step(first).status == XRFrameStatus::StaleSession);
            REQUIRE(runtime.LoadScript(second, script) == XRFrameStatus::StaleSession);
            const std::array fresh{Event(XRSessionEvent::Started), Frame(second)};
            REQUIRE(runtime.LoadScript(second, fresh) == XRFrameStatus::Ok);
            REQUIRE(runtime.Step(second).status == XRFrameStatus::Ok);
            REQUIRE(runtime.Step(second).frame.frame.sequence > old.frame.frame.sequence);
            runtime.Shutdown();
            runtime.Shutdown();
            REQUIRE(runtime.Step(second).status == XRFrameStatus::Shutdown);
            REQUIRE(runtime.LoadScript(second, fresh) == XRFrameStatus::Shutdown);
            REQUIRE(runtime.Snapshot().state == XRSessionState::Destroyed);
            REQUIRE(runtime.Activate(Capabilities(3), Plan(Capabilities(3)), {}).HasError());
        }
    }  // namespace
}  // namespace Horo::XR
