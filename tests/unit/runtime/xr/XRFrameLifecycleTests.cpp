#include "Horo/XR/XRFrameLifecycle.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace Horo::XR {
    namespace {
        template <typename T> T Generation(const std::uint64_t value) {
            auto created = T::Create(value);
            REQUIRE(created.HasValue());
            return created.Value();
        }

        XRCapabilitySnapshot Capabilities(const std::uint64_t runtime = 1, const std::uint64_t revision = 0) {
            XRCapabilityDescriptor descriptor{
                .system = {.runtime = Generation<XRRuntimeGeneration>(runtime), .slot = {.index = 2, .generation = 1}},
                .revision = Generation<XRCapabilityRevision>(revision == 0 ? runtime : revision),
                .limits = {.maximumViews = 4, .maximumSpaces = 8, .maximumActions = 8, .maximumDevices = 4},
            };
            descriptor.states.fill(XRCapabilityState::Unsupported);
            for (const XRCapability feature :
                 {XRCapability::Projection, XRCapability::PrimaryOpaqueStereo, XRCapability::OrientationTracking,
                  XRCapability::PositionTracking, XRCapability::ViewSpace, XRCapability::LocalSpace, XRCapability::BooleanActions,
                  XRCapability::FloatActions, XRCapability::Vector2Actions, XRCapability::PoseActions, XRCapability::PredictedFrames,
                  XRCapability::ExternalColorTargets, XRCapability::SessionLossLifecycle, XRCapability::CanonicalInputProjection})
                descriptor.states[static_cast<std::size_t>(feature)] = XRCapabilityState::Available;
            auto created = XRCapabilitySnapshot::Create(descriptor);
            REQUIRE(created.HasValue());
            return created.Value();
        }

        class NoopResources final : public IXRSessionResources {
        public:
            Result<void> Prepare(XRSessionPreparation, const XRSessionId &, const XRFeaturePlan &) override {
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation, const XRSessionId &) noexcept override {}
        };

        XRSessionId Activate(XRSessionLifecycle &sessions, const XRCapabilitySnapshot &capabilities) {
            constexpr XRFeatureNegotiationRequest projection{.profile = XRFeatureProfile::Projection1_0,
                                                             .requestedLimits = {.maximumViews = 2,
                                                                                 .maximumSpaces = 2,
                                                                                 .maximumActions = 4,
                                                                                 .maximumDevices = 1}};
            const auto plan = NegotiateXRFeatures(capabilities, capabilities.System(), capabilities.Revision(), projection);
            REQUIRE(plan.status == XRFeatureNegotiationStatus::Ok);
            auto result = sessions.Activate(capabilities, capabilities.System(), capabilities.Revision(), *plan.plan);
            REQUIRE(result.HasValue());
            const XRSessionId session = result.Value();
            REQUIRE(sessions.ApplyEvent(session, XRSessionEvent::Started).HasValue());
            return session;
        }

        XRViewConfigurationId Configuration(const XRSessionId &session, const std::uint32_t generation = 1) {
            return {session, {.index = 3, .generation = generation}};
        }

        XRSwapchainImageId Image(const XRSessionId &session, const std::uint32_t target, const std::uint32_t image = 1) {
            return {{session, {.index = target, .generation = 1}}, {.index = image, .generation = 1}};
        }

        constexpr XRFrameLimits StereoLimits{.maximumViews = 2, .maximumImages = 2, .maximumLayers = 1};

        XRRenderPredictionTime Prediction(const std::int64_t nanoseconds) {
            auto created = XRRenderPredictionTime::Create(nanoseconds);
            REQUIRE(created.HasValue());
            return created.Value();
        }

        XRFrameId Wait(XRFrameLifecycle &frames, const XRSessionId &session, const bool shouldRender = true) {
            const auto reserved = frames.ReserveWait(session);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            const auto waited = frames.RecordWait(Prediction(10'000), shouldRender);
            REQUIRE(waited.status == XRFrameStatus::Ok);
            REQUIRE(waited.frame.IsValid());
            REQUIRE(waited.frame == reserved.frame);
            return waited.frame;
        }

        TEST_CASE("XR frame contract completes a bounded predicted rendering transaction", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const auto allocations = Horo::Tests::AllocationProbe::Count();
            const XRFrameId frame = Wait(frames, session);
            REQUIRE(frame.capabilityRevision == capabilities.Revision());
            REQUIRE(frames.Snapshot().predictedDisplayTime == Prediction(10'000));
            REQUIRE(frames.Snapshot().shouldRender);
            REQUIRE(frames.Snapshot().renderAdmitted);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            const auto first = Image(session, 1);
            const auto second = Image(session, 2);
            REQUIRE(frames.Acquire(frame, first) == XRFrameStatus::Ok);
            REQUIRE(frames.Acquire(frame, second) == XRFrameStatus::Ok);
            const std::array images{first, second};
            REQUIRE(frames.Submit(frame, images) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(frame, second) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(frame, first) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 1) == XRFrameStatus::Ok);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Idle);
            REQUIRE(Horo::Tests::AllocationProbe::Count() == allocations);
            REQUIRE(frames.End(frame, 1) == XRFrameStatus::Duplicate);
        }

        TEST_CASE("XR should-render false still begins and ends with zero layers and no image work", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, {}) == XRFrameStatus::Ok);
            const auto reserved = frames.ReserveWait(session);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            const auto unsupported = frames.RecordWait(Prediction(10'000), true);
            REQUIRE(unsupported.status == XRFrameStatus::Unsupported);
            REQUIRE(unsupported.frame == reserved.frame);
            REQUIRE(frames.Snapshot().shouldRender);
            REQUIRE_FALSE(frames.Snapshot().renderAdmitted);
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Duplicate);
            REQUIRE(frames.Begin(unsupported.frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(unsupported.frame, 0) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session, false);
            REQUIRE_FALSE(frames.Snapshot().shouldRender);
            REQUIRE_FALSE(frames.Snapshot().renderAdmitted);
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Duplicate);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 1) == XRFrameStatus::Unsupported);
            REQUIRE(frames.Acquire(frame, Image(session, 1)) == XRFrameStatus::Unsupported);
            REQUIRE(frames.Submit(frame, {}) == XRFrameStatus::Unsupported);
            REQUIRE(frames.End(frame, 1) == XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR wait reservation rejects native work early and cancellation never reuses a frame", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            REQUIRE(frames.RecordWait(Prediction(10'000), true).status == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.CancelWait() == XRFrameStatus::OutOfOrder);

            const auto reserved = frames.ReserveWait(session);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::WaitReserved);
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Duplicate);
            REQUIRE(frames.Begin(reserved.frame) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.CancelWait() == XRFrameStatus::Ok);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Idle);
            REQUIRE(frames.RecordWait(Prediction(10'000), true).status == XRFrameStatus::OutOfOrder);

            const auto next = frames.ReserveWait(session);
            REQUIRE(next.status == XRFrameStatus::Ok);
            REQUIRE(next.frame.sequence > reserved.frame.sequence);
            const auto waited = frames.RecordWait(Prediction(20'000), false);
            REQUIRE(waited.status == XRFrameStatus::Ok);
            REQUIRE(waited.frame == next.frame);
            REQUIRE(frames.CancelWait() == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Begin(waited.frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(waited.frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame gate rejects skipped and duplicate operations without advancing state", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Acquire(frame, Image(session, 1)) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Waited);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Abort(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
            const XRFrameId next = Wait(frames, session);
            REQUIRE(next.sequence > frame.sequence);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::StaleFrame);
            REQUIRE(frames.Begin(next) == XRFrameStatus::Ok);
            REQUIRE(frames.Abort(next) == XRFrameStatus::Ok);
            REQUIRE(frames.End(next, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame admission rejects invalid unsupported stale and over-capacity inputs", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Unavailable);
            REQUIRE(frames.BindConfiguration({}, capabilities, StereoLimits) == XRFrameStatus::InvalidInput);
            REQUIRE(frames.BindConfiguration(Configuration(session), Capabilities(2, capabilities.Revision().Value()), StereoLimits) ==
                    XRFrameStatus::StaleSession);
            REQUIRE(frames.BindConfiguration(Configuration(session), Capabilities(1, 99), StereoLimits) == XRFrameStatus::StalePlan);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, {.maximumViews = 5}) == XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, {.maximumViews = 17}) ==
                    XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            REQUIRE(frames.ReserveWait({}).status == XRFrameStatus::InvalidInput);
            const auto reserved = frames.ReserveWait(session);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            const auto invalidPrediction = frames.RecordWait({}, true);
            REQUIRE(invalidPrediction.status == XRFrameStatus::InvalidInput);
            REQUIRE(invalidPrediction.frame == reserved.frame);
            REQUIRE_FALSE(frames.Snapshot().renderAdmitted);
            REQUIRE(frames.Begin(invalidPrediction.frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(invalidPrediction.frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame image and submission failures retain release obligations", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session);
            REQUIRE(frames.BindConfiguration(Configuration(session, 2), capabilities, StereoLimits) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 0) == XRFrameStatus::InvalidInput);
            REQUIRE(frames.LocateViews(frame, 3) == XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            const auto first = Image(session, 1);
            const auto second = Image(session, 2);
            REQUIRE(frames.Acquire(frame, {}) == XRFrameStatus::InvalidInput);
            XRSessionId foreign = session;
            ++foreign.slot.generation;
            REQUIRE(frames.Acquire(frame, Image(foreign, 1)) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Acquire(frame, first) == XRFrameStatus::Ok);
            REQUIRE(frames.Acquire(frame, first) == XRFrameStatus::Duplicate);
            REQUIRE(frames.Acquire(frame, second) == XRFrameStatus::Ok);
            REQUIRE(frames.Acquire(frame, Image(session, 3)) == XRFrameStatus::CapacityExceeded);
            const std::array partial{first};
            const std::array wrongOrder{second, first};
            const std::array foreignSubmission{Image(foreign, 1), second};
            REQUIRE(frames.Submit(frame, partial) == XRFrameStatus::IncompleteSubmission);
            REQUIRE(frames.Submit(frame, wrongOrder) == XRFrameStatus::IncompleteSubmission);
            REQUIRE(frames.Submit(frame, foreignSubmission) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::ImagesAcquired);
            REQUIRE(frames.Abort(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::ImageNotReleased);
            REQUIRE(frames.Release(frame, Image(foreign, 1)) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Release(frame, first) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(frame, first) == XRFrameStatus::Duplicate);
            REQUIRE(frames.Release(frame, second) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR session replacement retains completed wait debt until explicit quiescent reset", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto firstCapabilities = Capabilities();
            const XRSessionId firstSession = Activate(sessions, firstCapabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(firstSession), firstCapabilities, StereoLimits) == XRFrameStatus::Ok);
            const auto reserved = frames.ReserveWait(firstSession);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            const auto replacementCapabilities = Capabilities(2);
            const XRSessionId replacement = Activate(sessions, replacementCapabilities);
            const auto completed = frames.RecordWait(Prediction(10'000), true);
            REQUIRE(completed.status == XRFrameStatus::Ok);
            REQUIRE(completed.frame == reserved.frame);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Waited);
            REQUIRE(frames.Begin(completed.frame) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Waited);
            REQUIRE(frames.ReserveWait(replacement).status == XRFrameStatus::StaleSession);
            // The test resource port is quiescent; a real host must prove native wait/frame and lease retirement first.
            frames.ResetAfterQuiescence();
            REQUIRE(frames.BindConfiguration(Configuration(replacement), replacementCapabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId newFrame = Wait(frames, replacement);
            REQUIRE(newFrame.sequence > completed.frame.sequence);
            REQUIRE(frames.Begin(completed.frame) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Begin(newFrame) == XRFrameStatus::Ok);
            REQUIRE(frames.Abort(newFrame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(newFrame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame abort retains release obligations and shutdown closes admission", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            const auto image = Image(session, 1);
            REQUIRE(frames.Acquire(frame, image) == XRFrameStatus::Ok);
            REQUIRE(frames.Abort(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::ImageNotReleased);
            REQUIRE(frames.Release(frame, Image(session, 2)) == XRFrameStatus::ImageNotAcquired);
            REQUIRE(frames.Release(frame, image) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
            frames.Shutdown();
            frames.Shutdown();
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Shutdown);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Shutdown);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Shutdown);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Idle);
        }

        TEST_CASE("XR stopping session may finish an open frame but cannot admit another", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session, false);
            REQUIRE(sessions.ApplyEvent(session, XRSessionEvent::Stopping).HasValue());
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Unavailable);
        }
    }  // namespace
}  // namespace Horo::XR
