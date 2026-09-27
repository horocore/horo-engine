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
            descriptor.states[static_cast<std::size_t>(XRCapability::Projection)] = XRCapabilityState::Available;
            auto created = XRCapabilitySnapshot::Create(descriptor);
            REQUIRE(created.HasValue());
            return created.Value();
        }

        class NoopResources final : public IXRSessionResources {
        public:
            Result<void> Prepare(XRSessionPreparation, const XRSessionId &) override {
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation, const XRSessionId &) noexcept override {}
        };

        XRSessionId Activate(XRSessionLifecycle &sessions, const XRCapabilitySnapshot &capabilities) {
            constexpr XRCapabilityRequirement projection{.capability = XRCapability::Projection, .views = 2};
            auto result = sessions.Activate(capabilities, capabilities.System(), capabilities.Revision(), projection);
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
            const auto waited = frames.RecordWait(session, Prediction(10'000), shouldRender);
            REQUIRE(waited.status == XRFrameStatus::Ok);
            REQUIRE(waited.frame.IsValid());
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
            REQUIRE(frames.RecordWait(session, Prediction(10'000), true).status == XRFrameStatus::Unsupported);
            const XRFrameId frame = Wait(frames, session, false);
            REQUIRE_FALSE(frames.Snapshot().shouldRender);
            REQUIRE(frames.RecordWait(session, Prediction(11'000), false).status == XRFrameStatus::Duplicate);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 1) == XRFrameStatus::Unsupported);
            REQUIRE(frames.Acquire(frame, Image(session, 1)) == XRFrameStatus::Unsupported);
            REQUIRE(frames.Submit(frame, {}) == XRFrameStatus::Unsupported);
            REQUIRE(frames.End(frame, 1) == XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
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
            REQUIRE(frames.RecordWait(session, Prediction(10'000), true).status == XRFrameStatus::Unavailable);
            REQUIRE(frames.BindConfiguration({}, capabilities, StereoLimits) == XRFrameStatus::InvalidInput);
            REQUIRE(frames.BindConfiguration(Configuration(session), Capabilities(2, capabilities.Revision().Value()), StereoLimits) ==
                    XRFrameStatus::StaleSession);
            REQUIRE(frames.BindConfiguration(Configuration(session), Capabilities(1, 99), StereoLimits) == XRFrameStatus::StalePlan);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, {.maximumViews = 5}) == XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, {.maximumViews = 17}) ==
                    XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            REQUIRE(frames.RecordWait({}, Prediction(10'000), true).status == XRFrameStatus::InvalidInput);
            REQUIRE(frames.RecordWait(session, {}, true).status == XRFrameStatus::InvalidInput);
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

        TEST_CASE("XR session replacement fences old frames until explicit quiescent reset", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto firstCapabilities = Capabilities();
            const XRSessionId firstSession = Activate(sessions, firstCapabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(firstSession), firstCapabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId oldFrame = Wait(frames, firstSession);
            const auto replacementCapabilities = Capabilities(2);
            const XRSessionId replacement = Activate(sessions, replacementCapabilities);
            REQUIRE(frames.Begin(oldFrame) == XRFrameStatus::StaleSession);
            REQUIRE(frames.RecordWait(replacement, Prediction(20'000), true).status == XRFrameStatus::StaleSession);
            frames.ResetAfterQuiescence();
            REQUIRE(frames.BindConfiguration(Configuration(replacement), replacementCapabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId newFrame = Wait(frames, replacement);
            REQUIRE(newFrame.sequence > oldFrame.sequence);
            REQUIRE(frames.Begin(oldFrame) == XRFrameStatus::StaleSession);
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
            REQUIRE(frames.RecordWait(session, Prediction(30'000), true).status == XRFrameStatus::Shutdown);
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
            REQUIRE(frames.RecordWait(session, Prediction(20'000), false).status == XRFrameStatus::Unavailable);
        }
    }  // namespace
}  // namespace Horo::XR
