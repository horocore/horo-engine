#include "XRSimulatorState.h"

#include <algorithm>
#include <limits>

namespace Horo::XR {
    namespace {
        template <class Type> Type Value(const std::uint64_t value) {
            const auto made = Type::Create(value);
            HORO_INVARIANT(made.HasValue());
            return made.Value();
        }

        void Components(const XRSimulatorPose &pose, XRPoseDescriptor &descriptor) {
            using enum Horo::XR::XRTrackingLossState;
            if (pose.positionValid)
                descriptor.components.positionMeters = {pose.position, XRPoseComponentValidity::Tracked};
            if (pose.orientationValid)
                descriptor.components.orientation = {pose.orientation, XRPoseComponentValidity::Tracked};
            if (pose.positionValid || pose.orientationValid)
                descriptor.components.confidence = XRTrackingConfidence::High;
            if (pose.positionValid && pose.orientationValid)
                descriptor.components.loss = None;
            else if (pose.positionValid)
                descriptor.components.loss = OrientationLost;
            else if (pose.orientationValid)
                descriptor.components.loss = PositionLost;
        }

        XRPoseDescriptor Pose(const XRSimulatorPose &pose, const XRSessionId &session, const std::uint64_t time,
                              const XRSimulationTime simulation, const bool presentation, const bool head) {
            using enum Horo::XR::XRSpaceKind;
            const auto origin = Value<XRWorldOriginRevision>(1);
            XRPoseDescriptor descriptor{.session = session,
                                        .source = {{session, {1, 1}}, head ? View : Action, origin},
                                        .target = {{session, {2, 1}}, Local, origin},
                                        .purpose = presentation ? XRPosePurpose::PresentationPrediction : XRPosePurpose::SimulationInput,
                                        .time = {.runtimeSample = Value<XRRuntimeTime>(time)}};
            if (presentation)
                descriptor.time.renderPrediction = Value<XRRenderPredictionTime>(time + 1);
            else
                descriptor.time.simulation = simulation;
            Components(pose, descriptor);
            return descriptor;
        }

        const XRTrackedDeviceRecord *Device(const std::span<const XRTrackedDeviceRecord> devices, const XRTrackedDeviceRole role) noexcept {
            const auto found = std::ranges::find(devices, role, &XRTrackedDeviceRecord::role);
            return found == devices.end() ? nullptr : &*found;
        }

        XRFakeActionKind FakeKind(const XRTrackedActionKind kind) noexcept {
            using enum Horo::XR::XRFakeActionKind;
            constexpr std::array values{Boolean, Float, Vector2};
            return values[static_cast<std::size_t>(kind)];
        }

        bool SourceActive(const XRSimulatorSource &source) noexcept {
            if (source.role == XRTrackedDeviceRole::Head)
                return source.pose.positionValid || source.pose.orientationValid;
            return source.state != XRTrackedDeviceState::Disconnected;
        }

        XRTrackingState State(const std::span<const XRTrackedDeviceRecord> devices) noexcept {
            return std::ranges::any_of(devices,
                                       [](const auto &device) {
                return device.state == XRTrackedDeviceState::Tracked;
            })
                       ? XRTrackingState::Active
                       : XRTrackingState::TrackingLost;
        }

        XRSimulatorStep Event(const XRSessionEvent event) {
            XRSimulatorStep step;
            step.event = event;
            step.executeFrame = false;
            step.shouldRender = false;
            step.deltaNanoseconds = 0;
            return step;
        }
    }  // namespace

    Result<XRFakeStep> XRSimulatorRuntime::Impl::Compile(const XRSimulatorStep &step, const std::uint64_t nextTime) const {
        const auto session = host.Snapshot().session;
        XRFakeStep result{.event = step.event,
                          .executeFrame = step.executeFrame,
                          .shouldRender = step.shouldRender,
                          .failure = step.failure};
        if (!step.executeFrame)
            return Result<XRFakeStep>::Success(result);
        result.prediction = Value<XRRenderPredictionTime>(nextTime + 1);
        result.actionSampleTime = Value<XRRuntimeTime>(nextTime);
        result.viewCount = step.viewCount;
        for (std::size_t index = 0; index < step.viewCount; ++index) {
            const auto sample = XRPoseSample::Create(Pose(step.views[index], session, nextTime, simulationTime, true, true), session,
                                                     Value<XRWorldOriginRevision>(1));
            if (sample.HasError())
                return Result<XRFakeStep>::Failure(sample.ErrorValue());
            result.views[index] = sample.Value();
        }
        result.actionCount = step.actionCount;
        for (std::size_t index = 0; index < step.actionCount; ++index) {
            const auto &action = step.actions[index];
            result.actions[index] = {{session, {action.bindingIndex + 1, 1}}, FakeKind(action.kind), {action.x, action.y}, action.active};
        }
        return Result<XRFakeStep>::Success(result);
    }

    Result<void> XRSimulatorRuntime::Impl::Disconnect(const XRSimulatorStep &step) {
        const auto session = host.Snapshot().session;
        for (const auto &source : std::span{step.sources}.first(step.sourceCount)) {
            if (source.state != XRTrackedDeviceState::Disconnected)
                continue;
            const auto *device = Device(host.Devices(), source.role);
            if (!device)
                continue;
            const auto changed = host.ObserveDevice(session, source.role, XRTrackedDeviceState::Disconnected, device->capabilities);
            if (changed.HasError())
                return Result<void>::Failure(changed.ErrorValue());
        }
        return Result<void>::Success();
    }

    Result<void> XRSimulatorRuntime::Impl::Publish(const XRSimulatorStep &step, const XRFakeOutcome &outcome) {
        if (!outcome.session.admitsActions) {
            host.RevokeTracking();
            return Result<void>::Success();
        }
        const auto session = outcome.session.session;
        const auto origin = Value<XRWorldOriginRevision>(1);
        std::array<RuntimeInternal::TrackingSourceEvidence, XRSimulatorHardLimits::MaximumSources> sources;
        for (std::size_t index = 0; index < step.sourceCount; ++index) {
            const auto &source = step.sources[index];
            sources[index] = {source.role, source.kind, SourceActive(source),
                              Pose(source.pose, session, time, simulationTime, false, source.role == XRTrackedDeviceRole::Head)};
        }
        if (auto observed = host.ObserveSources(session, std::span{sources}.first(step.sourceCount)); observed.HasError())
            return observed;
        if (auto disconnected = Disconnect(step); disconnected.HasError())
            return disconnected;
        return PublishRecords(step, outcome, sources);
    }

    Result<void> XRSimulatorRuntime::Impl::PublishRecords(
        const XRSimulatorStep &step, const XRFakeOutcome &outcome,
        const std::array<RuntimeInternal::TrackingSourceEvidence, XRSimulatorHardLimits::MaximumSources> &sources) {
        const auto session = outcome.session.session;
        const auto origin = Value<XRWorldOriginRevision>(1);
        std::array<RuntimeInternal::TrackingPoseEvidence, XRSimulatorHardLimits::MaximumSources> poses;
        std::size_t count{};
        for (std::size_t index = 0; index < step.sourceCount; ++index) {
            const auto &source = step.sources[index];
            const auto *device = Device(host.Devices(), source.role);
            if (device && source.state == XRTrackedDeviceState::Tracked)
                poses[count++] = {device->id, source.kind, sources[index].pose};
        }
        std::array<XRTrackedActionRecord, XRFakeLimits::MaximumActions> actions{};
        const auto actionCount = ActionRecords(step, outcome, actions);
        std::array<XRHandJointRecord, XRHandTrackingHardLimits::MaximumJoints> joints{};
        if (auto result = JointRecords(step, joints); result.HasError())
            return result;
        const RuntimeInternal::TrackingPublicationRequest request{{outcome.actionSampleTime, simulationTime, {}},
                                                                  origin,
                                                                  XRTrackingFocusState::Focused,
                                                                  outcome.session.state == XRSessionState::Focused
                                                                      ? XRTrackingVisibilityState::Visible
                                                                      : XRTrackingVisibilityState::Hidden,
                                                                  State(host.Devices()),
                                                                  activeBindings.revision,
                                                                  std::span{poses}.first(count),
                                                                  std::span{actions}.first(actionCount),
                                                                  std::span{joints}.first(step.jointCount)};
        return host.Publish(session, request);
    }

    std::size_t XRSimulatorRuntime::Impl::ActionRecords(const XRSimulatorStep &step, const XRFakeOutcome &outcome,
                                                        std::array<XRTrackedActionRecord, XRFakeLimits::MaximumActions> &records) const {
        std::size_t count{};
        for (std::size_t index = 0; index < step.actionCount; ++index) {
            const auto &action = step.actions[index];
            const auto *device = Device(host.Devices(), action.role);
            if (!device)
                continue;
            const auto &actual = outcome.actions[index];
            records[count++] = {device->id, action.bindingIndex, action.kind, actual.active, actual.value[0], actual.value[1]};
        }
        return count;
    }

    Result<void> XRSimulatorRuntime::Impl::JointRecords(
        const XRSimulatorStep &step, std::array<XRHandJointRecord, XRHandTrackingHardLimits::MaximumJoints> &records) const {
        const auto session = host.Snapshot().session;
        const auto origin = Value<XRWorldOriginRevision>(1);
        for (std::size_t index = 0; index < step.jointCount; ++index) {
            const auto &joint = step.joints[index];
            const auto role = joint.hand == XRHandedness::Left ? XRTrackedDeviceRole::LeftHand : XRTrackedDeviceRole::RightHand;
            const auto *device = Device(host.Devices(), role);
            if (!device)
                return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
            const auto sample = XRPoseSample::Create(Pose(joint.pose, session, time, simulationTime, false, false), session, origin);
            if (sample.HasError())
                return Result<void>::Failure(sample.ErrorValue());
            records[index] = {device->id, joint.hand, joint.joint, sample.Value(), joint.radiusMeters};
        }
        return Result<void>::Success();
    }

    Result<XRSimulatorAdvance> XRSimulatorRuntime::Impl::Advance(const XRSimulatorCutoff hostCutoff) {
        if (hostCutoff.serial == 0 || hostCutoff.serial <= cutoff || !hostCutoff.simulation.IsValid())
            return Result<XRSimulatorAdvance>::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (auto bindings = CheckBindings(); bindings.HasError()) {
            host.RevokeTracking();
            playback = XRSimulatorPlayback::Failed;
            EndCapture(XRSimulatorCaptureState::Partial);
            return Result<XRSimulatorAdvance>::Failure(bindings.ErrorValue());
        }
        if (replay && (!scenario || cursor == scenario->Steps().size())) {
            playback = XRSimulatorPlayback::Paused;
            return Result<XRSimulatorAdvance>::Success({.cursor = cursor, .relativeNanoseconds = time});
        }
        const auto step = replay ? scenario->Steps()[cursor] : pendingEvent ? Event(*pendingEvent) : controls;
        return Consume(step, hostCutoff);
    }

    Result<XRSimulatorAdvance> XRSimulatorRuntime::Impl::Consume(const XRSimulatorStep &step, const XRSimulatorCutoff hostCutoff) {
        if (auto bindings = ValidateControlBindings(step); bindings.HasError())
            return Result<XRSimulatorAdvance>::Failure(bindings.ErrorValue());
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 1;
        if (step.deltaNanoseconds > maximum - time)
            return Result<XRSimulatorAdvance>::Failure(MakeError(XRErrors::CapacityExceeded));
        simulationTime = hostCutoff.simulation;
        const auto nextTime = time + step.deltaNanoseconds;
        const auto compiled = Compile(step, nextTime);
        if (compiled.HasError())
            return Result<XRSimulatorAdvance>::Failure(compiled.ErrorValue());
        const auto stepped = host.Step(host.Snapshot().session, compiled.Value());
        if (stepped.HasError())
            return Result<XRSimulatorAdvance>::Failure(stepped.ErrorValue());
        const auto &actual = stepped.Value();
        if (!actual.consumed) {
            host.RevokeTracking();
            EndCapture(XRSimulatorCaptureState::Partial);
            playback = XRSimulatorPlayback::Failed;
            return Result<XRSimulatorAdvance>::Success({actual, false, cursor, time});
        }
        return CommitConsumption(step, hostCutoff, actual, nextTime);
    }

    Result<XRSimulatorAdvance> XRSimulatorRuntime::Impl::CommitConsumption(const XRSimulatorStep &step, const XRSimulatorCutoff hostCutoff,
                                                                           const XRFakeOutcome &actual, const std::uint64_t nextTime) {
        time = nextTime;
        cutoff = hostCutoff.serial;
        ++cursor;
        Record(step);
        if (!replay) {
            pendingEvent.reset();
            controls.event.reset();
            controls.failure = XRFakeFailure::None;
        }
        if (actual.status == XRFrameStatus::Ok && step.executeFrame)
            if (auto published = Publish(step, actual); published.HasError()) {
                playback = XRSimulatorPlayback::Failed;
                EndCapture(XRSimulatorCaptureState::Partial);
                host.RevokeTracking();
                return Result<XRSimulatorAdvance>::Failure(published.ErrorValue());
            }
        if (actual.session.state == XRSessionState::Lost) {
            playback = XRSimulatorPlayback::Failed;
            EndCapture(XRSimulatorCaptureState::Partial);
        }
        return Result<XRSimulatorAdvance>::Success({actual, true, cursor, time});
    }
}  // namespace Horo::XR
