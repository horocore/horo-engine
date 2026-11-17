#include "support/AllocationProbe.h"
#include "support/xr/XRSimulatorTestSupport.h"

#include <algorithm>

namespace {
    using namespace Horo::XR;
    using namespace Horo::XR::SimulatorFeatureTests;

    TEST_CASE("Simulator run pause step reset and replay share genuine canonical sessions", "[xr][simulator][runtime]") {
        auto runtime = Runtime();
        const auto initial = runtime->Snapshot().session.session;
        REQUIRE(runtime->Snapshot().session.state == XRSessionState::Focused);
        REQUIRE_FALSE(runtime->Tick(Cutoff(1)).Value().consumed);
        REQUIRE(runtime->Run().HasValue());
        REQUIRE(runtime->Tick(Cutoff(1)).Value().consumed);
        REQUIRE(runtime->Tracking() != nullptr);
        REQUIRE(runtime->Pause().HasValue());
        REQUIRE_FALSE(runtime->Tick(Cutoff(2)).Value().consumed);
        REQUIRE(runtime->Step(Cutoff(2)).Value().consumed);
        REQUIRE(runtime->Step(Cutoff(2)).HasError());
        REQUIRE(runtime->Reset().HasValue());
        REQUIRE(runtime->Snapshot().session.session != initial);
        REQUIRE(runtime->Tracking() == nullptr);
        const std::array steps{runtime->Controls(), runtime->Controls()};
        REQUIRE(runtime->LoadScenario(Scenario(steps)).HasValue());
        const auto loaded = runtime->Snapshot().session.session;
        REQUIRE(runtime->Replay().HasValue());
        REQUIRE(runtime->Snapshot().session.session != loaded);
        REQUIRE(runtime->Tick(Cutoff(1)).Value().consumed);
        REQUIRE(runtime->Tick(Cutoff(2)).Value().consumed);
        REQUIRE_FALSE(runtime->Tick(Cutoff(3)).Value().consumed);
        REQUIRE(runtime->Snapshot().playback == XRSimulatorPlayback::Paused);
    }

    TEST_CASE("Simulator focus tracking loss and terminal events revoke prior publication", "[xr][simulator][runtime]") {
        auto runtime = Runtime();
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        REQUIRE(runtime->Tracking() != nullptr);
        REQUIRE(runtime->SetControls(Event(XRSessionEvent::FocusLost)).HasValue());
        REQUIRE(runtime->Step(Cutoff(2)).HasValue());
        REQUIRE(runtime->Tracking() == nullptr);
        REQUIRE(runtime->Snapshot().session.state == XRSessionState::Visible);
        REQUIRE(runtime->SetControls(Event(XRSessionEvent::BecameFocused)).HasValue());
        REQUIRE(runtime->Step(Cutoff(3)).HasValue());
        REQUIRE(runtime->Step(Cutoff(4)).HasValue());
        REQUIRE(runtime->Tracking() != nullptr);
        REQUIRE(runtime->SetControls(Event(XRSessionEvent::SessionLost)).HasValue());
        REQUIRE(runtime->Step(Cutoff(5)).HasValue());
        REQUIRE(runtime->Tracking() == nullptr);
        REQUIRE(runtime->Snapshot().playback == XRSimulatorPlayback::Failed);
        REQUIRE(runtime->Run().HasError());
        REQUIRE(runtime->Reset().HasValue());
    }

    TEST_CASE("Simulator rejects unsupported view count without partially acquiring images", "[xr][simulator][runtime]") {
        auto runtime = Runtime();
        auto controls = runtime->Controls();
        controls.viewCount = 3;
        REQUIRE(runtime->SetControls(controls).HasValue());
        const auto outcome = runtime->Step(Cutoff(1));
        REQUIRE(outcome.HasValue());
        REQUIRE(outcome.Value().frame.status == XRFrameStatus::CapacityExceeded);
        REQUIRE(outcome.Value().frame.frame.acquiredImages == 0);
        REQUIRE(runtime->Tracking() == nullptr);
    }

    TEST_CASE("Simulator owner incarnations and canonical replacement fence retained snapshots", "[xr][simulator][identity]") {
        auto first = Runtime();
        auto second = Runtime();
        REQUIRE(first->Step(Cutoff(1)).HasValue());
        const auto retained = *first->Tracking();
        const auto original = first->Snapshot().session.session;
        const auto other = second->Snapshot().session.session;
        REQUIRE(original.system.runtime != other.system.runtime);
        REQUIRE(original.system.runtime.Value() >= (std::uint64_t{1} << 32));
        REQUIRE(ValidateXRTrackingSnapshot(retained, other, retained.Revision(), retained.WorldOriginRevision()).HasError());
        REQUIRE(first->Reset().HasValue());
        const auto replacement = first->Snapshot().session.session;
        REQUIRE(replacement.system.runtime == original.system.runtime);
        REQUIRE(replacement != original);
        REQUIRE(ValidateXRTrackingSnapshot(retained, replacement, retained.Revision(), retained.WorldOriginRevision()).HasError());
        first.reset();
        auto recreated = Runtime();
        REQUIRE(recreated->Snapshot().session.session.system.runtime != original.system.runtime);
        REQUIRE(
            ValidateXRTrackingSnapshot(retained, recreated->Snapshot().session.session, retained.Revision(), retained.WorldOriginRevision())
                .HasError());
    }

    TEST_CASE("Simulator queued old-session events never cross successful replacement", "[xr][simulator][lifecycle]") {
        auto runtime = Runtime();
        const std::array steps{runtime->Controls()};
        REQUIRE(runtime->SetControls(Event(XRSessionEvent::FocusLost)).HasValue());
        REQUIRE(runtime->LoadScenario(Scenario(steps)).HasValue());
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        REQUIRE(runtime->Snapshot().session.state == XRSessionState::Focused);
        REQUIRE(runtime->SetControls(Event(XRSessionEvent::FocusLost)).HasValue());
        REQUIRE(runtime->Replay().HasValue());
        REQUIRE(runtime->Pause().HasValue());
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        REQUIRE(runtime->Snapshot().session.state == XRSessionState::Focused);
    }

    TEST_CASE("Simulator prepared step tracking and interaction recording allocate no C++ storage", "[xr][simulator][allocation]") {
        auto runtime = Runtime();
        REQUIRE(runtime->ArmCapture({.maximumSteps = 2}).HasValue());
        const auto cutoff = Cutoff(1);
        const auto before = Horo::Tests::AllocationProbe::Count();
        const auto advanced = runtime->Step(cutoff);
        const auto after = Horo::Tests::AllocationProbe::Count();
        REQUIRE(advanced.HasValue());
        REQUIRE(advanced.Value().consumed);
        REQUIRE(after == before);
        REQUIRE(runtime->Tracking() != nullptr);
        REQUIRE(runtime->Snapshot().capture.steps == 1);
    }

    TEST_CASE("Simulator anatomical hands use canonical joint publication and loss fencing", "[xr][simulator][hands]") {
        auto runtime = Runtime();
        auto controls = runtime->Controls();
        controls.sourceCount = 4;
        controls.sources[3] = {XRTrackedDeviceRole::LeftHand, XRTrackedPoseKind::Palm, XRTrackedDeviceState::Tracked, {}};
        controls.jointCount = 1;
        controls.joints[0] = {XRHandedness::Left, XRHandJoint::Palm, {}, 0.01F};
        REQUIRE(runtime->SetControls(controls).HasValue());
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        const auto *tracking = runtime->Tracking();
        REQUIRE(tracking != nullptr);
        REQUIRE(tracking->HandJoints().size() == 1);
        const auto retained = *tracking;
        const auto device = tracking->HandJoints()[0].device;
        REQUIRE(
            QueryXRHandJoint(retained, device, XRHandJoint::Palm, retained.Session(), retained.Revision(), retained.WorldOriginRevision())
                .HasValue());
        controls.jointCount = 0;
        controls.sources[3].state = XRTrackedDeviceState::TrackingLost;
        controls.sources[3].pose.positionValid = false;
        controls.sources[3].pose.orientationValid = false;
        REQUIRE(runtime->SetControls(controls).HasValue());
        REQUIRE(runtime->Step(Cutoff(2)).HasValue());
        REQUIRE(runtime->Tracking()->HandJoints().empty());
        REQUIRE(QueryXRHandJoint(retained, device, XRHandJoint::Palm, retained.Session(), runtime->Tracking()->Revision(),
                                 retained.WorldOriginRevision())
                    .HasError());
    }

    TEST_CASE("Simulator Head tracking loss normalizes source activity independently from live controller poses",
              "[xr][simulator][runtime]") {
        auto runtime = Runtime();
        REQUIRE(runtime->Step(Cutoff(1)).HasValue());
        auto controls = runtime->Controls();
        controls.sources[0].state = XRTrackedDeviceState::TrackingLost;
        controls.sources[0].pose.positionValid = false;
        controls.sources[0].pose.orientationValid = false;
        REQUIRE(runtime->SetControls(controls).HasValue());
        const auto advanced = runtime->Step(Cutoff(2));
        INFO((advanced.HasError() ? advanced.ErrorValue().message : "success"));
        REQUIRE(advanced.HasValue());
        REQUIRE(advanced.Value().consumed);
        const auto *tracking = runtime->Tracking();
        REQUIRE(tracking != nullptr);
        const auto devices = tracking->Devices();
        const auto head = std::ranges::find(devices, XRTrackedDeviceRole::Head, &XRTrackedDeviceRecord::role);
        const auto controller = std::ranges::find(devices, XRTrackedDeviceRole::LeftController, &XRTrackedDeviceRecord::role);
        REQUIRE(head != devices.end());
        REQUIRE(head->state == XRTrackedDeviceState::TrackingLost);
        REQUIRE(controller != devices.end());
        REQUIRE(controller->state == XRTrackedDeviceState::Tracked);
        REQUIRE(QueryXRTrackedPose(*tracking, head->id, XRTrackedPoseKind::Head, tracking->Session(), tracking->Revision(),
                                   tracking->WorldOriginRevision())
                    .HasError());
        REQUIRE(QueryXRTrackedPose(*tracking, controller->id, XRTrackedPoseKind::Grip, tracking->Session(), tracking->Revision(),
                                   tracking->WorldOriginRevision())
                    .HasValue());
    }
}  // namespace
