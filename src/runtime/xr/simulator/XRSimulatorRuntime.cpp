#include "XRSimulatorState.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace Horo::XR {
    namespace {
        template <class Type> Type Value(const std::uint64_t value) {
            const auto result = Type::Create(value);
            HORO_INVARIANT(result.HasValue());
            return result.Value();
        }

        Result<XRRuntimeGeneration> IssueIncarnation() {
            // One shared cold issuer for both factories. Native application attempts occupy the lower 32-bit domain.
            static std::atomic<std::uint64_t> next{std::uint64_t{1} << 32};
            auto candidate = next.load(std::memory_order_relaxed);
            while (candidate != std::numeric_limits<std::uint64_t>::max()) {
                if (next.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed))
                    return XRRuntimeGeneration::Create(candidate);
            }
            return Result<XRRuntimeGeneration>::Failure(MakeError(XRErrors::CapacityExceeded));
        }

        XRCapabilitySnapshot Capabilities(const XRRuntimeGeneration incarnation) {
            XRCapabilityDescriptor descriptor{.system = {incarnation, {0, 1}},
                                              .revision = Value<XRCapabilityRevision>(1),
                                              .limits = {2, 8, XRFakeLimits::MaximumActions, 5}};
            descriptor.states.fill(XRCapabilityState::Unsupported);
            constexpr std::array supported{XRCapability::Projection,
                                           XRCapability::PrimaryOpaqueStereo,
                                           XRCapability::OrientationTracking,
                                           XRCapability::PositionTracking,
                                           XRCapability::ViewSpace,
                                           XRCapability::LocalSpace,
                                           XRCapability::BooleanActions,
                                           XRCapability::FloatActions,
                                           XRCapability::Vector2Actions,
                                           XRCapability::PoseActions,
                                           XRCapability::PredictedFrames,
                                           XRCapability::ExternalColorTargets,
                                           XRCapability::SessionLossLifecycle,
                                           XRCapability::CanonicalInputProjection,
                                           XRCapability::HandTracking};
            for (const auto capability : supported)
                descriptor.states[static_cast<std::size_t>(capability)] = XRCapabilityState::Available;
            return XRCapabilitySnapshot::Create(descriptor).Value();
        }

        XRFeaturePlan Plan(const XRCapabilitySnapshot &capabilities) {
            const std::array hands{XRFeatureRequest{XRCapability::HandTracking, XRFeatureRequirementKind::Required}};
            const auto result = NegotiateXRFeatures(capabilities, capabilities.System(), capabilities.Revision(),
                                                    {.profile = XRFeatureProfile::Projection1_0,
                                                     .requestedLimits = capabilities.Limits(),
                                                     .features = hands});
            HORO_INVARIANT(result.plan.has_value());
            return *result.plan;
        }

        XRSimulatorStep DefaultControls() {
            XRSimulatorStep step;
            step.viewCount = 2;
            step.views[0].position = {-0.032F, 1.6F, 3};
            step.views[1].position = {0.032F, 1.6F, 3};
            step.sourceCount = 3;
            step.sources[0] = {XRTrackedDeviceRole::Head, XRTrackedPoseKind::Head, XRTrackedDeviceState::Tracked, {}};
            step.sources[0].pose.position = {0, 1.6F, 3};
            step.sources[1] = {XRTrackedDeviceRole::LeftController, XRTrackedPoseKind::Grip, XRTrackedDeviceState::Tracked, {}};
            step.sources[1].pose.position = {-0.2F, 1.2F, 2.7F};
            step.sources[2] = {XRTrackedDeviceRole::RightController, XRTrackedPoseKind::Grip, XRTrackedDeviceState::Tracked, {}};
            step.sources[2].pose.position = {0.2F, 1.2F, 2.7F};
            return step;
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

    XRSimulatorRuntime::Impl::Impl(const XRRuntimeGeneration ownerIncarnation, IXRSessionResources *resources,
                                   std::function<const XRActionBindingCoordinator *()> bindings)
        : incarnation(ownerIncarnation), children(resources), query(std::move(bindings)), controls(DefaultControls()) {}

    XRSimulatorRuntime::Impl::~Impl() {
        Close();
    }

    Result<void> XRSimulatorRuntime::Impl::Lane() const {
        if (std::this_thread::get_id() != lane || busy)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (shutdown || shutdownRequested)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<void>::Success();
    }

    void XRSimulatorRuntime::Impl::Finish() noexcept {
        busy = false;
        if (shutdownRequested)
            Close();
    }

    void XRSimulatorRuntime::Impl::Close() noexcept {
        if (std::this_thread::get_id() != lane || shutdown)
            return;
        if (busy) {
            shutdownRequested = true;
            return;
        }
        busy = true;
        EndCapture(XRSimulatorCaptureState::Partial);
        host.Shutdown();
        playback = XRSimulatorPlayback::Stopped;
        shutdown = true;
        shutdownRequested = false;
        busy = false;
    }

    Result<void> XRSimulatorRuntime::Impl::Bootstrap(const XRSimulatorInitialState initial) {
        constexpr std::array events{XRSessionEvent::Started, XRSessionEvent::BecameVisible, XRSessionEvent::BecameFocused};
        const auto session = host.Snapshot().session;
        for (std::size_t index = 0; index < static_cast<std::size_t>(initial); ++index) {
            XRFakeStep step{.event = events[index], .executeFrame = false, .shouldRender = false};
            const auto outcome = host.Step(session, step);
            if (outcome.HasError())
                return Result<void>::Failure(outcome.ErrorValue());
            if (outcome.Value().status != XRFrameStatus::Ok)
                return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        }
        return Result<void>::Success();
    }

    Result<void> XRSimulatorRuntime::Impl::Activate(const XRSimulatorInitialState initial) {
        const auto capabilities = Capabilities(incarnation);
        const auto budget = query ? static_cast<std::uint32_t>(XRFakeLimits::MaximumActions) : 0U;
        const auto activated = host.Activate(capabilities, Plan(capabilities), {2, 1, 1},
                                             {5, 7, budget, XRHandTrackingHardLimits::MaximumJoints}, Value<XRWorldOriginRevision>(1));
        if (activated.HasError())
            return Result<void>::Failure(activated.ErrorValue());
        activeBindings = candidateBindings;
        candidateBindings = {};
        if (auto started = Bootstrap(initial); started.HasError())
            return started;
        cursor = 0;
        time = 0;
        cutoff = 0;
        playback = XRSimulatorPlayback::Paused;
        return Result<void>::Success();
    }

    XRSimulatorRuntime::XRSimulatorRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    XRSimulatorRuntime::~XRSimulatorRuntime() {
        Shutdown();
    }

    /** @copydoc XRSimulatorRuntime::Create */
    Result<std::unique_ptr<XRSimulatorRuntime>> XRSimulatorRuntime::Create() {
        const auto incarnation = IssueIncarnation();
        if (incarnation.HasError())
            return Result<std::unique_ptr<XRSimulatorRuntime>>::Failure(incarnation.ErrorValue());
        auto impl = std::make_unique<Impl>(incarnation.Value());
        Impl::Operation operation{*impl};
        if (auto active = impl->Activate(XRSimulatorInitialState::Focused); active.HasError())
            return Result<std::unique_ptr<XRSimulatorRuntime>>::Failure(active.ErrorValue());
        // End the operation before moving the implementation; operation retains the same allocated owner.
        return Result<std::unique_ptr<XRSimulatorRuntime>>::Success(
            std::unique_ptr<XRSimulatorRuntime>{new XRSimulatorRuntime(std::move(impl))});
    }

    /** @copydoc RuntimeInternal::XRSimulatorHostAccess::Create */
    Result<std::unique_ptr<XRSimulatorRuntime>> RuntimeInternal::XRSimulatorHostAccess::Create(XRSimulatorHostComposition composition) {
        const auto incarnation = IssueIncarnation();
        if (incarnation.HasError())
            return Result<std::unique_ptr<XRSimulatorRuntime>>::Failure(incarnation.ErrorValue());
        auto impl =
            std::make_unique<XRSimulatorRuntime::Impl>(incarnation.Value(), &composition.resources, std::move(composition.bindings));
        XRSimulatorRuntime::Impl::Operation operation{*impl};
        if (auto active = impl->Activate(XRSimulatorInitialState::Focused); active.HasError())
            return Result<std::unique_ptr<XRSimulatorRuntime>>::Failure(active.ErrorValue());
        return Result<std::unique_ptr<XRSimulatorRuntime>>::Success(
            std::unique_ptr<XRSimulatorRuntime>{new XRSimulatorRuntime(std::move(impl))});
    }

    /** @copydoc XRSimulatorRuntime::LoadScenario */
    Result<void> XRSimulatorRuntime::LoadScenario(const XRSimulatorScenario &input) {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        Impl::Operation operation{*impl_};
        auto candidate = input;  // Bounded cold copy precedes any replacement.
        const auto prior = impl_->expectedFingerprint;
        impl_->expectedFingerprint = candidate.BindingFingerprint();
        if (auto activated = impl_->Activate(candidate.InitialState()); activated.HasError()) {
            impl_->expectedFingerprint = prior;
            return activated;
        }
        impl_->EndCapture(XRSimulatorCaptureState::Partial);
        impl_->scenario = std::move(candidate);
        impl_->pendingEvent.reset();
        impl_->controls.event.reset();
        impl_->controls.failure = XRFakeFailure::None;
        impl_->replay = false;
        return Result<void>::Success();
    }

    /** @copydoc XRSimulatorRuntime::SetControls */
    Result<void> XRSimulatorRuntime::SetControls(const XRSimulatorStep &controls) {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        if (auto shape = ValidateXRSimulatorStep(controls); shape.HasError())
            return shape;
        if (auto binding = impl_->ValidateControlBindings(controls); binding.HasError())
            return binding;
        if (!controls.executeFrame)
            impl_->pendingEvent = controls.event;
        else
            impl_->controls = controls;
        return Result<void>::Success();
    }

    /** @copydoc XRSimulatorRuntime::Run */
    Result<void> XRSimulatorRuntime::Run() {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        if (impl_->playback == XRSimulatorPlayback::Failed || impl_->playback == XRSimulatorPlayback::Stopped)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        impl_->playback = impl_->replay ? XRSimulatorPlayback::Replaying : XRSimulatorPlayback::Running;
        return Result<void>::Success();
    }

    /** @copydoc XRSimulatorRuntime::Pause */
    Result<void> XRSimulatorRuntime::Pause() {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        if (impl_->playback == XRSimulatorPlayback::Failed || impl_->playback == XRSimulatorPlayback::Stopped)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        impl_->playback = XRSimulatorPlayback::Paused;
        return Result<void>::Success();
    }

    /** @copydoc XRSimulatorRuntime::Replay */
    Result<void> XRSimulatorRuntime::Replay() {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        if (!impl_->scenario)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        Impl::Operation operation{*impl_};
        if (auto active = impl_->Activate(impl_->scenario->InitialState()); active.HasError())
            return active;
        impl_->EndCapture(XRSimulatorCaptureState::Partial);
        impl_->pendingEvent.reset();
        impl_->controls.event.reset();
        impl_->controls.failure = XRFakeFailure::None;
        impl_->replay = true;
        impl_->playback = XRSimulatorPlayback::Replaying;
        return Result<void>::Success();
    }

    /** @copydoc XRSimulatorRuntime::Reset */
    Result<void> XRSimulatorRuntime::Reset() {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        Impl::Operation operation{*impl_};
        const auto initial = impl_->scenario ? impl_->scenario->InitialState() : XRSimulatorInitialState::Focused;
        if (auto active = impl_->Activate(initial); active.HasError())
            return active;
        impl_->EndCapture(XRSimulatorCaptureState::Partial);
        impl_->replay = false;
        impl_->controls = DefaultControls();
        impl_->pendingEvent.reset();
        return Result<void>::Success();
    }

    /** @copydoc XRSimulatorRuntime::Stop */
    Result<void> XRSimulatorRuntime::Stop() {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        Impl::Operation operation{*impl_};
        const auto session = impl_->host.Snapshot().session;
        if (impl_->host.Snapshot().admitsFrames) {
            const auto stopping =
                impl_->host.Step(session, {.event = XRSessionEvent::Stopping, .executeFrame = false, .shouldRender = false});
            if (stopping.HasError())
                return Result<void>::Failure(stopping.ErrorValue());
            if (stopping.Value().consumed)
                impl_->Record(Event(XRSessionEvent::Stopping));
        }
        impl_->host.RevokeTracking();
        impl_->EndCapture(XRSimulatorCaptureState::Partial);
        impl_->playback = XRSimulatorPlayback::Stopped;
        return Result<void>::Success();
    }

    /** @copydoc XRSimulatorRuntime::Tick */
    Result<XRSimulatorAdvance> XRSimulatorRuntime::Tick(const XRSimulatorCutoff cutoff) {
        if (auto lane = impl_->Lane(); lane.HasError())
            return Result<XRSimulatorAdvance>::Failure(lane.ErrorValue());
        if (impl_->playback != XRSimulatorPlayback::Running && impl_->playback != XRSimulatorPlayback::Replaying)
            return Result<XRSimulatorAdvance>::Success({.cursor = impl_->cursor, .relativeNanoseconds = impl_->time});
        Impl::Operation operation{*impl_};
        return impl_->Advance(cutoff);
    }

    /** @copydoc XRSimulatorRuntime::Step */
    Result<XRSimulatorAdvance> XRSimulatorRuntime::Step(const XRSimulatorCutoff cutoff) {
        if (auto lane = impl_->Lane(); lane.HasError())
            return Result<XRSimulatorAdvance>::Failure(lane.ErrorValue());
        if (impl_->playback != XRSimulatorPlayback::Paused)
            return Result<XRSimulatorAdvance>::Failure(MakeError(XRErrors::OperationIncompatible));
        Impl::Operation operation{*impl_};
        return impl_->Advance(cutoff);
    }

    /** @copydoc XRSimulatorRuntime::Snapshot */
    XRSimulatorSnapshot XRSimulatorRuntime::Snapshot() const noexcept {
        if (std::this_thread::get_id() != impl_->lane)
            return {};
        return {impl_->playback, impl_->host.Snapshot(), impl_->cursor, impl_->time, impl_->capture};
    }

    /** @copydoc XRSimulatorRuntime::Tracking */
    const XRTrackingSnapshot *XRSimulatorRuntime::Tracking() const noexcept {
        return impl_->host.Current();
    }

    /** @copydoc XRSimulatorRuntime::Controls */
    const XRSimulatorStep &XRSimulatorRuntime::Controls() const noexcept {
        return impl_->controls;
    }

    /** @copydoc XRSimulatorRuntime::Shutdown */
    void XRSimulatorRuntime::Shutdown() noexcept {
        if (impl_)
            impl_->Close();
    }
}  // namespace Horo::XR
