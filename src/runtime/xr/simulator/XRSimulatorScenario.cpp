#include "Horo/XR/XRSimulatorScenario.h"

#include "XRSimulatorCodecInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::XR {
    namespace XRSimulatorErrors {
        const ErrorDomainId Domain{"xr.simulator"};
        const ErrorCodeDescriptor Invalid{Domain,
                                          ErrorCode{"xr.simulator.invalid"},
                                          ErrorSeverity::Error,
                                          "The simulator scenario is invalid.",
                                          "Use bounded canonical simulator controls and event values.",
                                          false,
                                          false};
        const ErrorCodeDescriptor VersionUnsupported{Domain,
                                                     ErrorCode{"xr.simulator.version_unsupported"},
                                                     ErrorSeverity::Error,
                                                     "The simulator artifact version is unsupported.",
                                                     "Use the supported simulator scenario schema.",
                                                     false,
                                                     false};
        const ErrorCodeDescriptor ArtifactInvalid{Domain,
                                                  ErrorCode{"xr.simulator.artifact_invalid"},
                                                  ErrorSeverity::Error,
                                                  "The simulator artifact is malformed or exceeds parsing limits.",
                                                  "Check strict fields and finite artifact budgets.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor CaptureUnavailable{Domain,
                                                     ErrorCode{"xr.simulator.capture_unavailable"},
                                                     ErrorSeverity::Error,
                                                     "The requested simulator capture is unavailable.",
                                                     "Record admitted simulator interaction within finite bounds.",
                                                     false,
                                                     false};
    }  // namespace XRSimulatorErrors

    namespace {
        Result<void> Invalid() {
            return Result<void>::Failure(MakeError(XRSimulatorErrors::Invalid));
        }

        bool Finite(const Math::Vec3 &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool Finite(const Math::Quaternion &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
        }

        bool ValidPose(const XRSimulatorPose &pose) noexcept {
            const auto &q = pose.orientation;
            const float length = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
            return Finite(pose.position) && Finite(q) && (!pose.orientationValid || std::abs(length - 1.0F) <= 0.0001F);
        }

        bool HandRole(const XRTrackedDeviceRole role) noexcept {
            return role == XRTrackedDeviceRole::LeftHand || role == XRTrackedDeviceRole::RightHand;
        }

        bool ControllerRole(const XRTrackedDeviceRole role) noexcept {
            return role == XRTrackedDeviceRole::LeftController || role == XRTrackedDeviceRole::RightController;
        }

        bool SourceKind(const XRSimulatorSource &source) noexcept {
            if (source.role == XRTrackedDeviceRole::Head)
                return source.kind == XRTrackedPoseKind::Head;
            if (ControllerRole(source.role))
                return source.kind == XRTrackedPoseKind::Grip || source.kind == XRTrackedPoseKind::Aim;
            return HandRole(source.role) && source.kind == XRTrackedPoseKind::Palm;
        }

        bool SourceState(const XRSimulatorSource &source) noexcept {
            if (source.state >= XRTrackedDeviceState::Count)
                return false;
            const bool located = source.pose.positionValid || source.pose.orientationValid;
            return (source.state == XRTrackedDeviceState::Tracked) == located;
        }

        Result<void> ValidateSources(const XRSimulatorStep &step) {
            for (std::size_t index = 0; index < step.sourceCount; ++index) {
                const auto &source = step.sources[index];
                if (!SourceKind(source) || !SourceState(source) || !ValidPose(source.pose))
                    return Invalid();
                if (index == 0)
                    continue;
                const auto &prior = step.sources[index - 1];
                if (!(std::pair{prior.role, prior.kind} < std::pair{source.role, source.kind}))
                    return Invalid();
                if (prior.role == source.role && prior.state != source.state)
                    return Invalid();
            }
            return Result<void>::Success();
        }

        const XRSimulatorSource *FindSource(const XRSimulatorStep &step, const XRTrackedDeviceRole role) noexcept {
            const auto sources = std::span{step.sources}.first(step.sourceCount);
            const auto found = std::ranges::find(sources, role, &XRSimulatorSource::role);
            return found == sources.end() ? nullptr : &*found;
        }

        bool Normalized(const XRSimulatorAction &action) noexcept {
            return std::isfinite(action.x) && std::isfinite(action.y) && std::abs(action.x) <= 1 && std::abs(action.y) <= 1;
        }

        bool ActionValue(const XRSimulatorAction &action) noexcept {
            if (!Normalized(action))
                return false;
            if (!action.active && (action.x != 0 || action.y != 0))
                return false;
            if (action.kind != XRTrackedActionKind::Vector2 && action.y != 0)
                return false;
            return action.kind != XRTrackedActionKind::Boolean || action.x == 0 || action.x == 1;
        }

        bool ActionShape(const XRSimulatorAction &action) noexcept {
            return action.bindingIndex < XRSimulatorHardLimits::MaximumActions && action.kind < XRTrackedActionKind::Count &&
                   ControllerRole(action.role) && ActionValue(action);
        }

        Result<void> ValidateActions(const XRSimulatorStep &step) {
            for (std::size_t index = 0; index < step.actionCount; ++index) {
                const auto &action = step.actions[index];
                if (!ActionShape(action))
                    return Invalid();
                if (const auto *source = FindSource(step, action.role);
                    !source || (action.active && source->state == XRTrackedDeviceState::Disconnected))
                    return Invalid();
                if (index > 0 && step.actions[index - 1].bindingIndex >= action.bindingIndex)
                    return Invalid();
            }
            return Result<void>::Success();
        }

        bool Radius(const std::optional<float> &radius) noexcept {
            return !radius || (std::isfinite(*radius) && *radius >= 0);
        }

        bool JointShape(const XRSimulatorJoint &joint) noexcept {
            return joint.hand < XRHandedness::Count && joint.joint < XRHandJoint::Count && ValidPose(joint.pose) &&
                   Radius(joint.radiusMeters);
        }

        Result<void> ValidateJoints(const XRSimulatorStep &step) {
            for (std::size_t index = 0; index < step.jointCount; ++index) {
                const auto &joint = step.joints[index];
                if (!JointShape(joint))
                    return Invalid();
                const auto role = joint.hand == XRHandedness::Left ? XRTrackedDeviceRole::LeftHand : XRTrackedDeviceRole::RightHand;
                if (const auto *source = FindSource(step, role); !source || source->state != XRTrackedDeviceState::Tracked)
                    return Invalid();
                if (index > 0 &&
                    !(std::pair{step.joints[index - 1].hand, step.joints[index - 1].joint} < std::pair{joint.hand, joint.joint}))
                    return Invalid();
            }
            return Result<void>::Success();
        }

        bool Counts(const XRSimulatorStep &step) noexcept {
            return step.viewCount <= XRFrameHardLimits::MaximumViews && step.sourceCount <= XRSimulatorHardLimits::MaximumSources &&
                   step.actionCount <= XRSimulatorHardLimits::MaximumActions && step.jointCount <= XRHandTrackingHardLimits::MaximumJoints;
        }

        bool EventOnly(const XRSimulatorStep &step) noexcept {
            return step.event && !step.shouldRender && step.failure == XRFakeFailure::None && step.deltaNanoseconds == 0 &&
                   step.viewCount == 0 && step.sourceCount == 0 && step.actionCount == 0 && step.jointCount == 0;
        }

        Result<void> ValidateFrame(const XRSimulatorStep &step) {
            if (step.deltaNanoseconds == 0 || step.deltaNanoseconds >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return Invalid();
            if (step.shouldRender && step.viewCount == 0)
                return Invalid();
            for (const auto &view : std::span{step.views}.first(step.viewCount))
                if (!ValidPose(view))
                    return Invalid();
            if (auto sources = ValidateSources(step); sources.HasError())
                return sources;
            if (auto actions = ValidateActions(step); actions.HasError())
                return actions;
            return ValidateJoints(step);
        }
    }  // namespace

    /** @copydoc ValidateXRSimulatorStep */
    Result<void> ValidateXRSimulatorStep(const XRSimulatorStep &step) {
        if (!Counts(step))
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if ((step.event && *step.event > XRSessionEvent::Ready) || step.failure >= XRFakeFailure::Count)
            return Invalid();
        if (!step.executeFrame)
            return EventOnly(step) ? Result<void>::Success() : Invalid();
        return ValidateFrame(step);
    }

    namespace {
        bool ValidCaptureBounds(const XRSimulatorCaptureManifest &capture) noexcept {
            return capture.maximumSteps > 0 && capture.maximumSteps <= XRSimulatorHardLimits::MaximumSteps &&
                   capture.maximumEncodedBytes >= SimulatorInternal::CaptureManifestCharge &&
                   capture.maximumEncodedBytes <= XRSimulatorHardLimits::MaximumEncodedBytes && capture.maximumDurationNanoseconds > 0 &&
                   capture.maximumDurationNanoseconds <= XRSimulatorHardLimits::MaximumCaptureDurationNanoseconds;
        }

        bool WithinCapture(const XRSimulatorCaptureManifest &capture, const XRSimulatorScenarioDescriptor &descriptor,
                           const std::uint64_t elapsed, const std::size_t bytes, const std::size_t nodes) noexcept {
            return descriptor.steps.size() <= capture.maximumSteps && bytes <= capture.maximumEncodedBytes &&
                   elapsed == capture.elapsedNanoseconds && elapsed <= capture.maximumDurationNanoseconds &&
                   nodes <= XRSimulatorHardLimits::MaximumNodes;
        }

        Result<void> ValidateManifest(const XRSimulatorScenarioDescriptor &descriptor, const std::uint64_t elapsed, const std::size_t bytes,
                                      const std::size_t nodes) {
            if (descriptor.provenance == XRSimulatorProvenance::SyntheticScenario)
                return descriptor.capture ? Invalid() : Result<void>::Success();
            if (!descriptor.capture)
                return Invalid();
            if (descriptor.complete && descriptor.droppedSteps != 0)
                return Invalid();
            const auto &capture = *descriptor.capture;
            if (!ValidCaptureBounds(capture))
                return Invalid();
            if (!WithinCapture(capture, descriptor, elapsed, bytes, nodes))
                return Invalid();
            return Result<void>::Success();
        }

        Result<void> ValidateScenarioSteps(const XRSimulatorScenarioDescriptor &descriptor) {
            std::uint64_t elapsed{};
            std::size_t bytes = SimulatorInternal::CaptureManifestCharge;
            std::size_t nodes = SimulatorInternal::CaptureManifestNodeCharge;
            constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 1;
            for (const auto &step : descriptor.steps) {
                if (auto valid = ValidateXRSimulatorStep(step); valid.HasError())
                    return valid;
                if (step.deltaNanoseconds > maximum - elapsed)
                    return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
                elapsed += step.deltaNanoseconds;
                const auto measured = SimulatorInternal::Measure(step);
                if (!measured.valid)
                    return Invalid();
                bytes += measured.bytes + 1;
                nodes += measured.nodes;
            }
            if (bytes > XRSimulatorHardLimits::MaximumEncodedBytes || nodes > XRSimulatorHardLimits::MaximumNodes)
                return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
            return ValidateManifest(descriptor, elapsed, bytes, nodes);
        }
    }  // namespace

    XRSimulatorScenario::XRSimulatorScenario(const XRSimulatorScenarioDescriptor &descriptor)
        : provenance_(descriptor.provenance), initialState_(descriptor.initialState),
          steps_(descriptor.steps.begin(), descriptor.steps.end()), complete_(descriptor.complete), droppedSteps_(descriptor.droppedSteps),
          bindings_(descriptor.bindings), capture_(descriptor.capture) {}

    /** @copydoc XRSimulatorScenario::Create */
    Result<XRSimulatorScenario> XRSimulatorScenario::Create(const XRSimulatorScenarioDescriptor &descriptor) {
        if (descriptor.version != XRSimulatorScenarioVersion)
            return Result<XRSimulatorScenario>::Failure(MakeError(XRSimulatorErrors::VersionUnsupported));
        if (descriptor.provenance >= XRSimulatorProvenance::Count || descriptor.initialState >= XRSimulatorInitialState::Count ||
            descriptor.steps.empty())
            return Result<XRSimulatorScenario>::Failure(MakeError(XRSimulatorErrors::Invalid));
        if (descriptor.steps.size() > XRSimulatorHardLimits::MaximumSteps)
            return Result<XRSimulatorScenario>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (const bool bound = std::ranges::any_of(descriptor.bindings,
                                                   [](const auto value) {
            return value != std::byte{};
        });
            !bound && std::ranges::any_of(descriptor.steps, [](const auto &step) {
            return step.actionCount != 0;
        }))
            return Result<XRSimulatorScenario>::Failure(MakeError(XRSimulatorErrors::Invalid));
        if (auto validated = ValidateScenarioSteps(descriptor); validated.HasError())
            return Result<XRSimulatorScenario>::Failure(validated.ErrorValue());
        return Result<XRSimulatorScenario>::Success(XRSimulatorScenario{descriptor});
    }

    /** @copydoc XRSimulatorScenario::Steps */
    std::span<const XRSimulatorStep> XRSimulatorScenario::Steps() const noexcept {
        return steps_;
    }

    /** @copydoc XRSimulatorScenario::Provenance */
    XRSimulatorProvenance XRSimulatorScenario::Provenance() const noexcept {
        return provenance_;
    }

    /** @copydoc XRSimulatorScenario::InitialState */
    XRSimulatorInitialState XRSimulatorScenario::InitialState() const noexcept {
        return initialState_;
    }

    /** @copydoc XRSimulatorScenario::Complete */
    bool XRSimulatorScenario::Complete() const noexcept {
        return complete_;
    }

    /** @copydoc XRSimulatorScenario::DroppedSteps */
    std::uint32_t XRSimulatorScenario::DroppedSteps() const noexcept {
        return droppedSteps_;
    }

    /** @copydoc XRSimulatorScenario::BindingFingerprint */
    const XRSimulatorBindingFingerprint &XRSimulatorScenario::BindingFingerprint() const noexcept {
        return bindings_;
    }

    /** @copydoc XRSimulatorScenario::CaptureManifest */
    const std::optional<XRSimulatorCaptureManifest> &XRSimulatorScenario::CaptureManifest() const noexcept {
        return capture_;
    }
}  // namespace Horo::XR
