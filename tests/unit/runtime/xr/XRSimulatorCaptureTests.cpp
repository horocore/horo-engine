#include "support/xr/XRSimulatorTestSupport.h"

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
