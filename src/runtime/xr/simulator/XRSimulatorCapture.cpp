#include "XRSimulatorState.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace Horo::XR {
    namespace {
        bool Active(const XRSimulatorCaptureState state) noexcept {
            return state == XRSimulatorCaptureState::Armed || state == XRSimulatorCaptureState::Capturing;
        }

        bool ValidLimits(const XRSimulatorCaptureLimits &limits) noexcept {
            return limits.maximumSteps > 0 && limits.maximumSteps <= XRSimulatorHardLimits::MaximumSteps &&
                   limits.maximumBytes >= SimulatorInternal::CaptureManifestCharge &&
                   limits.maximumBytes <= XRSimulatorHardLimits::MaximumEncodedBytes && limits.maximumDurationNanoseconds > 0 &&
                   limits.maximumDurationNanoseconds <= XRSimulatorHardLimits::MaximumCaptureDurationNanoseconds;
        }

        std::optional<XRSimulatorInitialState> Initial(const XRSessionState state) noexcept {
            switch (state) {
                case XRSessionState::Ready:
                    return XRSimulatorInitialState::Ready;
                case XRSessionState::Running:
                    return XRSimulatorInitialState::Running;
                case XRSessionState::Visible:
                    return XRSimulatorInitialState::Visible;
                case XRSessionState::Focused:
                    return XRSimulatorInitialState::Focused;
                default:
                    return {};
            }
        }
    }  // namespace

    Result<void> XRSimulatorRuntime::Impl::Arm(const XRSimulatorCaptureLimits limits) {
        if (limits.channel != XRSimulatorCaptureChannel::SimulatorControls)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        if (!ValidLimits(limits))
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (Active(capture.state))
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (auto bindings = CheckBindings(); bindings.HasError())
            return bindings;
        const auto initial = Initial(host.Snapshot().state);
        if (!initial)
            return Result<void>::Failure(MakeError(XRSimulatorErrors::CaptureUnavailable));
        std::vector<XRSimulatorStep> storage;
        storage.reserve(limits.maximumSteps);  // Only Arm allocates capture storage.
        recorded.swap(storage);
        captureLimits = limits;
        captureInitial = *initial;
        captureFingerprint = activeBindings.fingerprint;
        capture = {.state = XRSimulatorCaptureState::Armed, .bytes = SimulatorInternal::CaptureManifestCharge};
        recordedNodes = SimulatorInternal::CaptureManifestNodeCharge;
        return Result<void>::Success();
    }

    void XRSimulatorRuntime::Impl::Record(const XRSimulatorStep &step) noexcept {
        static_assert(std::is_nothrow_copy_constructible_v<XRSimulatorStep>);
        if (!Active(capture.state))
            return;
        const auto measured = SimulatorInternal::Measure(step);
        if (!FitsCapture(step, measured)) {
            capture.state = XRSimulatorCaptureState::Partial;
            ++capture.dropped;
            return;
        }
        recorded.push_back(step);
        capture.state = XRSimulatorCaptureState::Capturing;
        capture.steps = recorded.size();
        capture.bytes += measured.bytes + 1;
        capture.elapsedNanoseconds += step.deltaNanoseconds;
        recordedNodes += measured.nodes;
    }

    bool XRSimulatorRuntime::Impl::FitsCapture(const XRSimulatorStep &step,
                                               const SimulatorInternal::EncodedMeasure &measured) const noexcept {
        return measured.valid && recorded.size() < captureLimits.maximumSteps &&
               measured.bytes + 1 <= captureLimits.maximumBytes - capture.bytes &&
               step.deltaNanoseconds <= captureLimits.maximumDurationNanoseconds - capture.elapsedNanoseconds &&
               measured.nodes <= XRSimulatorHardLimits::MaximumNodes - recordedNodes;
    }

    void XRSimulatorRuntime::Impl::EndCapture(const XRSimulatorCaptureState state) noexcept {
        if (!Active(capture.state))
            return;
        capture.state = recorded.empty() ? XRSimulatorCaptureState::Cancelled : state;
        if (recorded.empty())
            capture = {.state = XRSimulatorCaptureState::Cancelled};
    }

    /** @copydoc XRSimulatorRuntime::ArmCapture */
    Result<void> XRSimulatorRuntime::ArmCapture(const XRSimulatorCaptureLimits limits) {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        Impl::Operation operation{*impl_};
        return impl_->Arm(limits);
    }

    /** @copydoc XRSimulatorRuntime::StopCapture */
    Result<void> XRSimulatorRuntime::StopCapture() {
        using enum Horo::XR::XRSimulatorCaptureState;
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        if (impl_->recorded.empty()) {
            impl_->capture = {.state = Cancelled};
            return Result<void>::Failure(MakeError(XRSimulatorErrors::CaptureUnavailable));
        }
        if (Active(impl_->capture.state))
            impl_->EndCapture(Complete);
        if (impl_->capture.state != Complete && impl_->capture.state != Partial)
            return Result<void>::Failure(MakeError(XRSimulatorErrors::CaptureUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc XRSimulatorRuntime::CancelCapture */
    void XRSimulatorRuntime::CancelCapture() noexcept {
        if (std::this_thread::get_id() != impl_->lane || impl_->busy)
            return;
        impl_->recorded.clear();
        impl_->capture = {.state = XRSimulatorCaptureState::Cancelled};
        impl_->recordedNodes = 0;
    }

    /** @copydoc XRSimulatorRuntime::RecordedScenario */
    Result<XRSimulatorScenario> XRSimulatorRuntime::RecordedScenario() const {
        using enum Horo::XR::XRSimulatorCaptureState;
        if (auto lane = impl_->Lane(); lane.HasError())
            return Result<XRSimulatorScenario>::Failure(lane.ErrorValue());
        const auto &capture = impl_->capture;
        if ((capture.state != Complete && capture.state != Partial) || impl_->recorded.empty())
            return Result<XRSimulatorScenario>::Failure(MakeError(XRSimulatorErrors::CaptureUnavailable));
        const XRSimulatorCaptureManifest manifest{static_cast<std::uint32_t>(impl_->captureLimits.maximumSteps),
                                                  impl_->captureLimits.maximumBytes, impl_->captureLimits.maximumDurationNanoseconds,
                                                  capture.elapsedNanoseconds};
        return XRSimulatorScenario::Create({.provenance = XRSimulatorProvenance::RecordedSimulatorInteraction,
                                            .initialState = impl_->captureInitial,
                                            .steps = impl_->recorded,
                                            .complete = capture.state == Complete,
                                            .droppedSteps = static_cast<std::uint32_t>(capture.dropped),
                                            .bindings = impl_->captureFingerprint,
                                            .capture = manifest});
    }
}  // namespace Horo::XR
