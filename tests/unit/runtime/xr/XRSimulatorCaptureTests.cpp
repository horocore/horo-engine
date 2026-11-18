#include "support/xr/XRSimulatorTestSupport.h"

#include <algorithm>

namespace {
    using namespace Horo::XR;
    using namespace Horo::XR::SimulatorFeatureTests;

    TEST_CASE("Simulator capture records admitted midrun interaction and replays fresh owners", "[xr][simulator][capture]") {
        auto runtime = Runtime();
        REQUIRE(runtime->ArmCapture().HasValue());
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        auto controls = runtime->Controls();
        controls.sources[0].pose.position.x = 4;
        REQUIRE(runtime->SetControls(controls).HasValue());
        REQUIRE(runtime->Step(Cutoff(2)).HasValue());
        REQUIRE(runtime->StopCapture().HasValue());
        auto captured = runtime->RecordedScenario();
        REQUIRE(captured.HasValue());
        REQUIRE(captured.Value().Provenance() == XRSimulatorProvenance::RecordedSimulatorInteraction);
        REQUIRE(captured.Value().Complete());
        REQUIRE(captured.Value().Steps().size() == 2);
        REQUIRE(captured.Value().Steps()[0].sources[0].pose.position.x == 0);
        REQUIRE(captured.Value().Steps()[1].sources[0].pose.position.x == 4);
        REQUIRE(captured.Value().CaptureManifest().has_value());
        const auto encoded = EncodeXRSimulatorScenario(captured.Value());
        REQUIRE(encoded.HasValue());
        auto imported = ParseXRSimulatorScenario(encoded.Value());
        REQUIRE(imported.HasValue());
        auto replay = Runtime();
        REQUIRE(replay->LoadScenario(imported.Value()).HasValue());
        REQUIRE(replay->Replay().HasValue());
        REQUIRE(replay->Tick(Cutoff(1)).Value().consumed);
        REQUIRE(replay->Tick(Cutoff(2)).Value().consumed);
        REQUIRE(replay->Snapshot().relativeNanoseconds == runtime->Snapshot().relativeNanoseconds);
        REQUIRE(replay->Snapshot().session.session != runtime->Snapshot().session.session);
    }

    TEST_CASE("Simulator finite capture exhaustion remains partial after artifact roundtrip", "[xr][simulator][capture]") {
        auto runtime = Runtime();
        XRSimulatorCaptureLimits limits;
        SECTION("step quota") {
            limits.maximumSteps = 1;
        }
        SECTION("duration quota") {
            limits.maximumDurationNanoseconds = runtime->Controls().deltaNanoseconds;
        }
        REQUIRE(runtime->ArmCapture(limits).HasValue());
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        REQUIRE(runtime->Step(Cutoff(2)).HasValue());
        REQUIRE(runtime->Snapshot().capture.state == XRSimulatorCaptureState::Partial);
        REQUIRE(runtime->Snapshot().capture.dropped == 1);
        auto captured = runtime->RecordedScenario();
        REQUIRE(captured.HasValue());
        REQUIRE_FALSE(captured.Value().Complete());
        REQUIRE(captured.Value().DroppedSteps() == 1);
        const auto encoded = EncodeXRSimulatorScenario(captured.Value());
        REQUIRE(encoded.HasValue());
        const auto imported = ParseXRSimulatorScenario(encoded.Value());
        REQUIRE(imported.HasValue());
        REQUIRE_FALSE(imported.Value().Complete());
        REQUIRE(imported.Value().DroppedSteps() == 1);
        REQUIRE(imported.Value().CaptureManifest()->elapsedNanoseconds == runtime->Controls().deltaNanoseconds);
    }

    TEST_CASE("Simulator records and replays a delayed frame and lifecycle faults through the same owners",
              "[xr][simulator][capture][faults]") {
        auto runtime = Runtime();
        const auto ordinary = runtime->Controls();

        auto delayed = ordinary;
        delayed.deltaNanoseconds *= 3;
        delayed.shouldRender = false;
        delayed.failure = XRFakeFailure::Wait;

        auto trackingLost = ordinary;
        trackingLost.sources[1].state = XRTrackedDeviceState::TrackingLost;
        trackingLost.sources[1].pose.positionValid = false;
        trackingLost.sources[1].pose.orientationValid = false;

        auto failedRelease = ordinary;
        failedRelease.failure = XRFakeFailure::Release;
        const auto terminal = Event(XRSessionEvent::SessionLost);
        using enum XRFrameStatus;

        struct ExpectedStep final {
            const XRSimulatorStep &controls;
            XRFrameStatus status{Ok};
            XRSessionState session{XRSessionState::Focused};
            std::uint32_t acquiredImages{1};
            std::optional<XRTrackedDeviceState> left{XRTrackedDeviceState::Tracked};
        };

        const std::array timeline{
            ExpectedStep{ordinary},
            ExpectedStep{delayed, Unavailable, XRSessionState::Focused, 0, std::nullopt},
            ExpectedStep{.controls = trackingLost, .left = XRTrackedDeviceState::TrackingLost},
            ExpectedStep{failedRelease, Unavailable, XRSessionState::Focused, 1, std::nullopt},
            ExpectedStep{ordinary},
            ExpectedStep{terminal, Ok, XRSessionState::Lost, 0, std::nullopt},
        };

        const auto checkStep = [&](XRSimulatorRuntime &owner, const bool replaying, const std::size_t index, const std::uint64_t elapsed) {
            CAPTURE(index);
            const auto cutoff = Cutoff(index + 1);
            const auto result = replaying ? owner.Tick(cutoff) : owner.Step(cutoff);
            REQUIRE(result.HasValue());
            const auto &advanced = result.Value();
            const auto &expected = timeline[index];
            CHECK(advanced.consumed);
            CHECK(advanced.cursor == index + 1);
            CHECK(advanced.relativeNanoseconds == elapsed);
            CHECK(advanced.frame.status == expected.status);
            CHECK(advanced.frame.failure == expected.controls.failure);
            CHECK(advanced.frame.session.state == expected.session);
            CHECK(advanced.frame.frame.acquiredImages == expected.acquiredImages);
            const auto *tracking = owner.Tracking();
            REQUIRE((tracking != nullptr) == expected.left.has_value());
            if (!tracking)
                return;
            CHECK(tracking->SampledAt() == advanced.frame.actionSampleTime);
            const auto left = std::ranges::find(tracking->Devices(), XRTrackedDeviceRole::LeftController, &XRTrackedDeviceRecord::role);
            REQUIRE(left != tracking->Devices().end());
            CHECK(left->state == *expected.left);
        };

        REQUIRE(runtime->ArmCapture().HasValue());
        std::uint64_t elapsed{};
        std::unique_ptr<XRTrackingSnapshot> originalTracking;
        for (std::size_t index = 0; index < timeline.size(); ++index) {
            REQUIRE(runtime->SetControls(timeline[index].controls).HasValue());
            elapsed += timeline[index].controls.deltaNanoseconds;
            checkStep(*runtime, false, index, elapsed);
            if (index == 0)
                originalTracking = std::make_unique<XRTrackingSnapshot>(*runtime->Tracking());
        }
        REQUIRE(runtime->Snapshot().playback == XRSimulatorPlayback::Failed);
        const auto captured = runtime->RecordedScenario();
        REQUIRE(captured.HasValue());
        REQUIRE_FALSE(captured.Value().Complete());
        REQUIRE(captured.Value().DroppedSteps() == 0);
        REQUIRE(captured.Value().CaptureManifest()->elapsedNanoseconds == elapsed);
        REQUIRE(captured.Value().Steps().size() == timeline.size());
        const auto encoded = EncodeXRSimulatorScenario(captured.Value());
        REQUIRE(encoded.HasValue());
        const auto decoded = ParseXRSimulatorScenario(encoded.Value());
        REQUIRE(decoded.HasValue());
        const auto reencoded = EncodeXRSimulatorScenario(decoded.Value());
        REQUIRE(reencoded.HasValue());
        CHECK(reencoded.Value() == encoded.Value());

        auto replay = Runtime();
        REQUIRE(replay->LoadScenario(decoded.Value()).HasValue());
        REQUIRE(replay->Replay().HasValue());
        REQUIRE(originalTracking != nullptr);
        const auto replaySession = replay->Snapshot().session.session;
        const auto stale = ValidateXRTrackingSnapshot(*originalTracking, replaySession, originalTracking->Revision(),
                                                      originalTracking->WorldOriginRevision());
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == XRErrors::IdentityStale.code.Value());
        elapsed = 0;
        for (std::size_t index = 0; index < timeline.size(); ++index) {
            elapsed += timeline[index].controls.deltaNanoseconds;
            checkStep(*replay, true, index, elapsed);
        }
        CHECK(replay->Snapshot().playback == XRSimulatorPlayback::Failed);
        replay->Shutdown();
        replay->Shutdown();
        CHECK(replay->Tracking() == nullptr);
        CHECK(replay->Tick(Cutoff(timeline.size() + 1)).HasError());
    }

    TEST_CASE("Simulator capture cancellation native channels and terminal failure retain truthful outcomes", "[xr][simulator][capture]") {
        auto runtime = Runtime();
        REQUIRE(runtime->ArmCapture({.channel = XRSimulatorCaptureChannel::NativeTracking}).HasError());
        REQUIRE(runtime->ArmCapture().HasValue());
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        runtime->CancelCapture();
        REQUIRE(runtime->Snapshot().capture.state == XRSimulatorCaptureState::Cancelled);
        REQUIRE(runtime->RecordedScenario().HasError());
        REQUIRE(runtime->ArmCapture().HasValue());
        REQUIRE(runtime->Step(Cutoff(2)).HasValue());
        REQUIRE(runtime->Stop().HasValue());
        REQUIRE(runtime->Snapshot().capture.state == XRSimulatorCaptureState::Partial);
        REQUIRE_FALSE(runtime->RecordedScenario().Value().Complete());
    }

    TEST_CASE("Simulator replay closes recording at genuine session discontinuity", "[xr][simulator][capture]") {
        auto runtime = Runtime();
        const std::array steps{runtime->Controls()};
        REQUIRE(runtime->LoadScenario(Scenario(steps)).HasValue());
        REQUIRE(runtime->SetControls(Event(XRSessionEvent::FocusLost)).HasValue());
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        REQUIRE(runtime->ArmCapture().HasValue());
        REQUIRE(runtime->Step(Cutoff(2)).HasValue());
        REQUIRE(runtime->Replay().HasValue());
        REQUIRE(runtime->Snapshot().capture.state == XRSimulatorCaptureState::Partial);
        REQUIRE(runtime->Tick(Cutoff(1)).HasValue());
        const auto captured = runtime->RecordedScenario();
        REQUIRE(captured.HasValue());
        REQUIRE(captured.Value().InitialState() == XRSimulatorInitialState::Visible);
        REQUIRE(captured.Value().Steps().size() == 1);
        REQUIRE_FALSE(captured.Value().Complete());
    }
}  // namespace
