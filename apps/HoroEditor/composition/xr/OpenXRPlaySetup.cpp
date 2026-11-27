#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/XR/XRSessionErrors.h"
#include "OpenXRPlayHost.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Application::Internal {
    /** @copydoc OpenXRPlayHost::StartNativeLighting */
    Result<void> OpenXRPlayHost::StartNativeLighting(const XR::XRLightEstimateScope &scope, const XR::XRLightEstimateLimits &limits,
                                                     XR::XRLightProjectionPolicy policy) {
        using namespace XR;
        if (const auto lane = Lane(); lane.HasError())
            return lane;
        if (!scene_ || !scene_->published || scene_->retiring || frames_.Snapshot().phase != XRFramePhase::Idle)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (composition_.lighting || !Editor::EditorVulkanXrViewportAccess::SupportsEnvironmentLighting(composition_.viewport))
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        if (!scope.IsValid() || scope.session != scene_->session || scope.space != scene_->worldReference)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (scene_->nativeLighting)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        policy.maximumAge = std::min(policy.maximumAge, limits.maximumAge);
        auto projection = XRLightProjection::Create(scope, policy);
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        const Operation operation{*this};
        return AdoptNativeLighting(scope, limits, std::move(projection).Value());
    }

    /** @copydoc OpenXRPlayHost::AdoptNativeLighting */
    Result<void> OpenXRPlayHost::AdoptNativeLighting(const XR::XRLightEstimateScope &scope, const XR::XRLightEstimateLimits &limits,
                                                     XR::XRLightProjection projection) {
        if (auto native = native_.StartLightEstimation(scope, limits, LightAuthorization()); native.HasError()) {
            if (native_.NeedsLightRetirement(scope))
                scene_->nativeLighting.emplace(scope, std::move(projection), false);
            return native;
        }
        const bool admitted = !pendingRevoke_ && LightingCutoff(scope).has_value();
        scene_->nativeLighting.emplace(scope, std::move(projection), admitted);
        if (admitted)
            return Result<void>::Success();
        auto denied = MakeError(XR::XRErrors::PermissionDenied);
        if (auto retired = RetireNativeLighting(); retired.HasError())
            return Result<void>::Failure(WithCause(std::move(retired).ErrorValue(), denied));
        return Result<void>::Failure(std::move(denied));
    }

    /** @copydoc OpenXRPlayHost::StopNativeLighting */
    Result<void> OpenXRPlayHost::StopNativeLighting() {
        if (const auto lane = Lane(); lane.HasError())
            return lane;
        if (!scene_ || !scene_->nativeLighting)
            return Result<void>::Success();
        scene_->nativeLighting->active = false;
        if (frames_.Snapshot().phase != XR::XRFramePhase::Idle)
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        const Operation operation{*this};
        return RetireNativeLighting();
    }

    /** @copydoc OpenXRPlayHost::ProgressNativeLighting */
    void OpenXRPlayHost::ProgressNativeLighting() {
        if (scene_ && scene_->nativeLighting && !scene_->nativeLighting->active &&
            frames_.Snapshot().phase == XR::XRFramePhase::Idle)
            (void)RetireNativeLighting();
    }

    /** @copydoc OpenXRPlayHost::RetireNativeLighting */
    Result<void> OpenXRPlayHost::RetireNativeLighting() {
        if (auto retired = native_.StopLightEstimation(scene_->nativeLighting->scope); retired.HasError())
            return retired;
        scene_->nativeLighting.reset();
        return Result<void>::Success();
    }

    namespace {
        constexpr std::chrono::milliseconds PreviewQueueLifetime{500};

        /** @brief Maps a validated controller hand into the canonical tracking role. */
        XR::XRTrackedDeviceRole ControllerRole(Editor::EditorXrHapticHand hand) noexcept {
            return hand == Editor::EditorXrHapticHand::Left ? XR::XRTrackedDeviceRole::LeftController
                                                            : XR::XRTrackedDeviceRole::RightController;
        }

        /** @brief Projects stable XR error identities without exposing native diagnostic text to a presentation caller. */
        Editor::EditorXrSetupIssue SetupIssue(const Error &error) noexcept {
            using enum Editor::EditorXrSetupIssue;
            using namespace XR;
            const auto &code = error.code.Value();
            if (code == XRSessionErrors::SetupCleanupPending.code.Value())
                return Cleanup;
            if (code == XRErrors::PermissionDenied.code.Value())
                return Permission;
            if (code == XRErrors::IdentityStale.code.Value() || code == XRErrors::CapabilityStale.code.Value() ||
                code == XRErrors::LoaderPreflightStale.code.Value())
                return Stale;
            if (code == XRErrors::OperationIncompatible.code.Value())
                return Renderer;
            if (code == XRErrors::OperationUnsupported.code.Value() || code == XRErrors::SystemUnsupported.code.Value())
                return Unsupported;
            if (code == XRErrors::CapacityExceeded.code.Value())
                return Capacity;
            if (code == XRErrors::OperationInvalid.code.Value())
                return Invalid;
            return Unavailable;
        }

        /** @brief Copies finite classifications only. Raw pose and joint fields are never read by the inspector. */
        void AddValidity(Editor::EditorXrInspectionValidity &out, const XR::XRPoseComponents &components) noexcept {
            static_assert(static_cast<std::size_t>(XR::XRPoseComponentValidity::Count) == decltype(out.position){}.size());
            static_assert(static_cast<std::size_t>(XR::XRTrackingConfidence::Count) == decltype(out.confidence){}.size());
            ++out.position[static_cast<std::size_t>(components.positionMeters.validity)];
            ++out.orientation[static_cast<std::size_t>(components.orientation.validity)];
            ++out.confidence[static_cast<std::size_t>(components.confidence)];
        }

        /** @brief Projects finite capability states from the accepted inspection plan. */
        void CopyInspectionCapabilities(Editor::EditorXrInspectionSnapshot &report, const XR::XRCapabilitySnapshot &capabilities,
                                        const XR::XRFeaturePlan &plan, XR::XRCapabilityState eyeGaze) noexcept {
            using enum XR::XRCapability;
            constexpr std::array kinds{Projection, ViewSpace, LocalSpace, StageSpace, CanonicalInputProjection, HandTracking, EyeGaze};
            static_assert(kinds.size() == Editor::EditorXrInspectionSnapshot::CapabilityCount);
            for (std::size_t index = 0; index < kinds.size(); ++index) {
                const auto kind = kinds[index];
                const auto observed = kind == EyeGaze ? eyeGaze : capabilities.State(kind);
                report.capabilities[index] = {kind, observed, plan.Decision(kind).state};
            }
        }

        /** @brief Classifies a native activation failure without exposing its platform message. */
        XR::XRCapabilityState PassthroughStartFailure(const Error &error) noexcept {
            const auto &denied = XR::XRErrors::PermissionDenied;
            return ErrorChainContains(error, denied.domain, denied.code) ? XR::XRCapabilityState::Denied
                                                                         : XR::XRCapabilityState::Unavailable;
        }
    }  // namespace

    /** @copydoc CopyOpenXRInspectionTracking */
    void CopyOpenXRInspectionTracking(Editor::EditorXrInspectionSnapshot &out, const XR::XRTrackingSnapshot &tracking) noexcept {
        static_assert(static_cast<std::size_t>(XR::XRSpaceKind::Count) == decltype(out.spaces){}.size());
        static_assert(XR::XRTrackingSnapshotHardLimits::MaximumActions <= std::numeric_limits<std::uint16_t>::max());
        out.poses = {};
        out.spaces = {};
        out.hands = {};
        out.actions = {};
        out.tracking = tracking.State();
        out.trackingRevision = tracking.Revision().Value();
        out.originRevision = tracking.WorldOriginRevision().Value();
        out.bindingRevision = tracking.BindingRevision();
        out.sampleNanoseconds = tracking.SampledAt().Nanoseconds();
        out.deviceCount = static_cast<std::uint16_t>(tracking.Devices().size());
        for (const auto &pose : tracking.Poses()) {
            AddValidity(out.poses, pose.sample.Components());
            ++out.spaces[static_cast<std::size_t>(pose.sample.Source().kind)];
            ++out.spaces[static_cast<std::size_t>(pose.sample.Target().kind)];
        }
        for (const auto &device : tracking.Devices()) {
            using enum XR::XRTrackedDeviceRole;
            if (device.role == LeftHand || device.role == RightHand) {
                auto &hand = out.hands[device.role == LeftHand ? 0 : 1];
                hand.state = device.state;
                hand.generation = device.id.slot.generation;
            }
        }
        for (const auto &joint : tracking.HandJoints())
            AddValidity(out.hands[static_cast<std::size_t>(joint.hand)].validity, joint.sample->Components());
        const auto devices = tracking.Devices();
        const auto actions = tracking.Actions();
        const auto count = std::min(actions.size(), out.actions.size());
        out.actionCount = static_cast<std::uint16_t>(actions.size());
        out.activeActionCount = static_cast<std::uint16_t>(std::ranges::count(actions, true, &XR::XRTrackedActionRecord::active));
        out.omittedActionCount = static_cast<std::uint16_t>(actions.size() - count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto &action = actions[index];
            const auto device = std::ranges::lower_bound(devices, action.device, {}, &XR::XRTrackedDeviceRecord::id);
            out.actions[index] = {.bindingIndex = action.bindingIndex,
                                  .deviceSlot = action.device.slot.index,
                                  .deviceGeneration = action.device.slot.generation,
                                  .role =
                                      device != devices.end() && device->id == action.device ? std::optional{device->role} : std::nullopt,
                                  .kind = action.kind,
                                  .active = action.active};
        }
    }

    /** @copydoc OpenXRPlayHost::SetupIdle */
    bool OpenXRPlayHost::SetupIdle() const noexcept {
        return std::this_thread::get_id() == lane_ && !closing_ && !busy_ && !instanceLost_ && !scene_ && !render_ && !events_ &&
               input_.play == Editor::EditorPlayState::Idle;
    }

    /** @copydoc Editor::IEditorXrSetup::ReadSetup */
    Editor::EditorXrSetupReport OpenXRPlayHost::ReadSetup() const noexcept {
        if (std::this_thread::get_id() != lane_ || closing_)
            return {};
        auto report = setup_;
        if (instanceLost_) {
            report.rendererReady = false;
            if (report.test == Editor::EditorXrSetupTestState::Passed)
                report.test = Editor::EditorXrSetupTestState::Failed;
            report.issue = Editor::EditorXrSetupIssue::Stale;
            return report;
        }
        if (setupRefreshRequested_) {
            if (report.test == Editor::EditorXrSetupTestState::Passed)
                report.test = Editor::EditorXrSetupTestState::Queued;
        } else if (setupPreflight_) {
            report.display = setupPreflight_->Compatibility().display;
        }
        report.mayRequestTest = SetupIdle() && queuedSetupTest_.value == 0;
        if (!report.mayRequestTest && report.test == Editor::EditorXrSetupTestState::NotRun &&
            report.issue == Editor::EditorXrSetupIssue::None)
            report.issue = Editor::EditorXrSetupIssue::Busy;
        return report;
    }

    /** @copydoc Editor::IEditorXrSetup::ReadInspection */
    Editor::EditorXrInspectionSnapshot OpenXRPlayHost::ReadInspection() const noexcept {
        using enum Editor::EditorXrInspectionStatus;
        Editor::EditorXrInspectionSnapshot report;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_)
            return report;

        const auto state = sessions_->Snapshot();
        if (instanceLost_ || state.state == XR::XRSessionState::Lost || (scene_ && scene_->retiring) ||
            (!state.session.IsValid() && retired_.IsValid())) {
            report.session = state.state;
            report.status = Stale;
            return report;
        }
        if (!state.session.IsValid() || !scene_ || !scene_->published) {
            report.status = Idle;
            return report;
        }

        report.session = state.state;
        // The owner only publishes an accepted plan and tracking sample from this exact session.
        const auto plan = sessions_->AcceptedPlan();
        if (!plan || !scene_->projection || scene_->session != state.session ||
            scene_->projection->capabilities.Revision() != state.capabilityRevision || plan->Revision() != state.capabilityRevision ||
            (scene_->geometry && !scene_->geometry->IsMountedSourceCurrent())) {
            report.status = Stale;
            return report;
        }
        report.runtimeGeneration = state.session.system.runtime.Value();
        report.systemGeneration = state.session.system.slot.generation;
        report.sessionGeneration = state.session.slot.generation;
        report.capabilityRevision = state.capabilityRevision.Value();
        report.focused = state.admitsActions && !eventsPending_;
        CopyInspectionCapabilities(report, scene_->projection->capabilities, *plan, composition_.session.EyeGazeAvailability());

        const auto routing = input_.router ? input_.router->RoutingState({}) : Input::InputRoutingState{};
        const bool currentInput = routing.configurationRevision != 0 && routing.configurationRevision == input_.inspectionConfiguration &&
                                  routing.assignmentRevision == input_.interactionAssignment;
        if (eventsPending_)
            report.status = EventsPending;
        else if (!state.admitsActions)
            report.status = Unfocused;
        else if (const auto *tracking = tracking_.Current(); tracking && tracking->Session() == state.session &&
                                                             tracking->WorldOriginRevision() == input_.origin && input_.admitted &&
                                                             currentInput && input_.play == Editor::EditorPlayState::Playing) {
            CopyOpenXRInspectionTracking(report, *tracking);
            report.stageBoundary = composition_.session.ReadStageBoundary(*tracking);
            if (const auto availability = composition_.session.ReadHandAvailability(*tracking)) {
                const auto hand =
                    std::ranges::find(report.capabilities, XR::XRCapability::HandTracking, &Editor::EditorXrInspectionCapability::kind);
                if (hand != report.capabilities.end())
                    hand->observed = *availability;
            }
            report.status = Current;
        } else
            report.status = WaitingForSample;
        return report;
    }

    /** @copydoc OpenXRPlayHost::CaptureHapticTarget */
    std::optional<OpenXRPlayHost::HapticTarget> OpenXRPlayHost::CaptureHapticTarget(Editor::EditorXrHapticHand hand) const noexcept {
        using enum Editor::EditorXrHapticHand;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || eventsPending_ || !input_.admitted ||
            input_.play != Editor::EditorPlayState::Playing || !input_.port || (hand != Left && hand != Right))
            return std::nullopt;
        const auto player = input_.port->AssignedPlayer();
        const auto *snapshot = tracking_.Current();
        const auto session = sessions_->Snapshot();
        if (!player || !snapshot || !session.admitsActions || !scene_ || !scene_->published || scene_->retiring ||
            snapshot->Session() != session.session || snapshot->BindingRevision() == 0)
            return std::nullopt;
        const auto role = ControllerRole(hand);
        if (!HasHapticRoute(role))
            return std::nullopt;
        const auto devices = snapshot->Devices();
        const auto found = std::ranges::find(devices, role, &XR::XRTrackedDeviceRecord::role);
        if (found == devices.end() || found->state != XR::XRTrackedDeviceState::Tracked)
            return std::nullopt;
        return HapticTarget{session.session,
                            found->id,
                            snapshot->BindingRevision(),
                            scene_->scene,
                            *player,
                            input_.port->ReadPlayerAssignmentRevision(),
                            role};
    }

    /** @copydoc OpenXRPlayHost::HasHapticRoute */
    bool OpenXRPlayHost::HasHapticRoute(const XR::XRTrackedDeviceRole role) const noexcept {
        const auto &routes = composition_.bindings->haptics;
        return std::ranges::find(routes, role, &OpenXRApplicationHaptic::role) != routes.end();
    }

    /** @copydoc Editor::IEditorXrHapticPreview::CanPreviewHaptic */
    bool OpenXRPlayHost::CanPreviewHaptic(Editor::EditorXrHapticHand hand) const noexcept {
        using enum Editor::EditorXrHapticPreviewState;
        const auto target = CaptureHapticTarget(hand);
        return target && target->assignment && (!hapticPreview_ || (hapticPreview_->state != Queued && !hapticPreview_->mayNeedStop));
    }

    /** @copydoc Editor::IEditorXrHapticPreview::QueueHapticPreview */
    Result<Editor::EditorXrHapticPreviewId> OpenXRPlayHost::QueueHapticPreview(const Editor::EditorXrHapticIntent intent) {
        using Reply = Result<Editor::EditorXrHapticPreviewId>;
        using namespace XR;
        if (const auto lane = Lane(); lane.HasError())
            return Reply::Failure(lane.ErrorValue());
        using enum Editor::EditorXrHapticPreviewState;
        using enum Editor::EditorXrHapticHand;
        if ((intent.hand != Left && intent.hand != Right) || !std::isfinite(intent.amplitude) || intent.amplitude < 0.0F ||
            intent.amplitude > 1.0F || intent.duration <= Duration{} || intent.duration > Editor::EditorXrHapticIntent::MaximumDuration)
            return Reply::Failure(MakeError(XRErrors::OperationInvalid));
        if (!HasHapticRoute(ControllerRole(intent.hand)))
            return Reply::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto target = CaptureHapticTarget(intent.hand);
        if (!target)
            return Reply::Failure(MakeError(XRErrors::OperationUnavailable));
        if (!HasCurrentAssignedInput())
            return Reply::Failure(MakeError(XRErrors::IdentityStale));
        if (!target->assignment)
            return Reply::Failure(MakeError(XRErrors::OperationUnavailable));
        if ((hapticPreview_ && (hapticPreview_->state == Queued || hapticPreview_->mayNeedStop)) ||
            nextHapticPreview_ == std::numeric_limits<std::uint64_t>::max())
            return Reply::Failure(MakeError(XRErrors::CapacityExceeded));
        const Editor::EditorXrHapticPreviewId receipt{++nextHapticPreview_};
        hapticPreview_ = HapticPreviewRecord{receipt, *target, intent, std::chrono::steady_clock::now() + PreviewQueueLifetime};
        return Reply::Success(receipt);
    }

    /** @copydoc Editor::IEditorXrHapticPreview::ReadHapticPreview */
    Editor::EditorXrHapticPreviewState OpenXRPlayHost::ReadHapticPreview(Editor::EditorXrHapticPreviewId receipt) const noexcept {
        if (std::this_thread::get_id() == lane_ && hapticPreview_ && receipt == hapticPreview_->receipt)
            return hapticPreview_->state;
        return Editor::EditorXrHapticPreviewState::Unknown;
    }

    /** @copydoc Editor::IEditorXrHapticPreview::CancelHapticPreview */
    void OpenXRPlayHost::CancelHapticPreview(Editor::EditorXrHapticPreviewId receipt) noexcept {
        if (std::this_thread::get_id() == lane_ && hapticPreview_ && receipt == hapticPreview_->receipt)
            hapticPreview_->cancelRequested = true;
    }

    /** @copydoc OpenXRPlayHost::PumpHapticPreview */
    void OpenXRPlayHost::PumpHapticPreview() {
        using enum Editor::EditorXrHapticPreviewState;
        if (std::this_thread::get_id() != lane_ || busy_ || !hapticPreview_)
            return;
        auto &preview = *hapticPreview_;
        if (preview.state != Queued && !preview.mayNeedStop)
            return;
        const auto now = std::chrono::steady_clock::now();
        const bool expired = now >= preview.until;
        const auto revoked = [&] {
            return preview.cancelRequested || CaptureHapticTarget(preview.intent.hand) != preview.target;
        };
        const bool lostOwner = revoked();
        if (expired || lostOwner) {
            if (preview.mayNeedStop && sessions_->Snapshot().session == preview.target.session)
                composition_.session.RequestHapticCancellation(preview.target.role);
            preview.mayNeedStop = false;
            if (lostOwner || preview.state == Queued)
                preview.state = Cancelled;
            return;
        }
        if (preview.state != Queued)
            return;

        const auto submitted = [&] {
            const Operation operation{*this};
            return composition_.session.SubmitHapticPreview(preview.target.session, preview.target.role, preview.intent.amplitude,
                                                            preview.intent.duration);
        }();
        if (revoked()) {
            if (sessions_->Snapshot().session == preview.target.session)
                composition_.session.RequestHapticCancellation(preview.target.role);
            preview.state = Cancelled;
            return;
        }
        if (submitted.HasError()) {
            const auto &error = submitted.ErrorValue();
            const auto &unsupported = XR::XRErrors::OperationUnsupported;
            preview.state = error.domain.Value() == unsupported.domain.Value() && error.code.Value() == unsupported.code.Value()
                                ? Unsupported
                                : Rejected;
            composition_.session.RequestHapticCancellation(preview.target.role);
            return;
        }
        preview.mayNeedStop = true;
        preview.until = std::chrono::steady_clock::now() + std::chrono::nanoseconds{preview.intent.duration.ToNanoseconds()};
        preview.state = Submitted;
    }

    /** @copydoc OpenXRPlayHost::ReadEyeGaze */
    std::optional<XR::XRPoseSample> OpenXRPlayHost::ReadEyeGaze() const noexcept {
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || eventsPending_ || !input_.admitted ||
            input_.play != Editor::EditorPlayState::Playing || !sessions_->Snapshot().admitsActions)
            return std::nullopt;
        if (composition_.session.EyeGazeAvailability() != XR::XRCapabilityState::Available)
            return std::nullopt;
        const auto now = clock_.RuntimeNow();
        const auto *snapshot = tracking_.Current();
        if (now.HasError() || !snapshot || closing_ || eventsPending_ || input_.play != Editor::EditorPlayState::Playing ||
            !sessions_->Snapshot().admitsActions)
            return std::nullopt;
        return composition_.session.ReadEyeGaze(*snapshot, now.Value());
    }

    /** @copydoc OpenXRPlayHost::ReadPlaneObservations */
    Result<XR::XRPlaneSnapshot> OpenXRPlayHost::ReadPlaneObservations() const {
        XR::XRRuntimeTime observedAt;
        return ReadPlaneObservationsAtCurrentTime(observedAt);
    }

    /** @copydoc OpenXRPlayHost::PlaneReadsAdmitted */
    bool OpenXRPlayHost::PlaneReadsAdmitted() const noexcept {
        return std::this_thread::get_id() == lane_ && !closing_ && !busy_ && !instanceLost_ && !eventsPending_ && input_.admitted &&
               input_.play == Editor::EditorPlayState::Playing && sessions_->Snapshot().admitsActions;
    }

    /** @copydoc OpenXRPlayHost::ReadPlaneObservationsAtCurrentTime */
    Result<XR::XRPlaneSnapshot> OpenXRPlayHost::ReadPlaneObservationsAtCurrentTime(XR::XRRuntimeTime &observedAt) const {
        using Outcome = Result<XR::XRPlaneSnapshot>;
        if (!PlaneReadsAdmitted())
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        const auto now = clock_.RuntimeNow();
        if (now.HasError())
            return Outcome::Failure(now.ErrorValue());
        if (!PlaneReadsAdmitted())
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        observedAt = now.Value();
        const auto *tracking = tracking_.Current();
        return tracking ? composition_.session.ReadPlaneObservations(*tracking, observedAt)
                        : Outcome::Failure(MakeError(XR::XRErrors::TrackingSnapshotStale));
    }

    /** @copydoc OpenXRPlayHost::QueryPlaneHits */
    Result<XR::XRSpatialHits> OpenXRPlayHost::QueryPlaneHits(const XR::XRSpatialHitRequest &request) const {
        XR::XRRuntimeTime observedAt;
        auto source = ReadPlaneObservationsAtCurrentTime(observedAt);
        if (source.HasError())
            return Result<XR::XRSpatialHits>::Failure(source.ErrorValue());
        auto liveRequest = request;
        if (request.now.IsValid() && request.now >= source.Value().SampledAt())
            liveRequest.now = std::max(request.now, observedAt);
        return XR::QueryXRPlaneHits(source.Value(), liveRequest);
    }

    /** @copydoc OpenXRPlayHost::ReadPressedPointer */
    std::optional<OpenXRWorldUiPointerFrame> OpenXRPlayHost::ReadPressedPointer(const OpenXRWorldUiPointerFrame &frame) const {
        const auto active = ReadWorldUiPointer(frame.source.player);
        if (!active || active->source != frame.source || active->cutoff != frame.cutoff || !active->sample.selectDown)
            return std::nullopt;
        return active;
    }

    /** @copydoc OpenXRPlayHost::QueryPointedPlanes */
    Result<XR::XRSpatialHits> OpenXRPlayHost::QueryPointedPlanes(const OpenXRWorldUiPointerFrame &frame,
                                                                 const std::uint32_t maximumHits) const {
        using Hits = Result<XR::XRSpatialHits>;
        if (!ReadPressedPointer(frame))
            return Hits::Failure(MakeError(XR::XRErrors::IdentityStale));
        XR::XRRuntimeTime observedAt;
        const auto source = ReadPlaneObservationsAtCurrentTime(observedAt);
        if (source.HasError())
            return Hits::Failure(source.ErrorValue());
        const auto active = ReadPressedPointer(frame);
        if (!active)
            return Hits::Failure(MakeError(XR::XRErrors::IdentityStale));
        const auto &snapshot = source.Value();
        if (snapshot.Scope().space != active->sample.space || snapshot.SampledAt() > active->cutoff.sampledAt)
            return Hits::Failure(MakeError(XR::XRErrors::TrackingSnapshotStale));
        // The publication owner already enforced its age at observedAt. Pin this exact admitted sample.
        const auto admittedAge =
            Duration::FromNanoseconds(std::max<std::int64_t>(1, observedAt.Nanoseconds() - snapshot.SampledAt().Nanoseconds()));
        return XR::QueryXRPlaneHits(snapshot,
                                    {snapshot.Scope(), active->sample.ray, observedAt, admittedAge, maximumHits, snapshot.SampledAt()});
    }

    /** @copydoc Editor::IEditorXrPlaneCapturePreview::ReadPlaneCaptureState */
    Editor::EditorXrPlaneCaptureState OpenXRPlayHost::ReadPlaneCaptureState() const noexcept {
        using enum Editor::EditorXrPlaneCaptureState;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || instanceLost_)
            return Unavailable;
        if (planesArmed_)
            return Armed;
        if (SetupIdle() && composition_.session.MayArmPlaneCapture())
            return Ready;
        if (!PlaneReadsAdmitted())
            return Unavailable;
        try {
            const auto now = clock_.RuntimeNow();
            if (now.HasError() || !PlaneReadsAdmitted())
                return Unavailable;
            switch (composition_.session.PlaneCaptureAvailability(now.Value())) {
                case XR::XRCapabilityState::Available:
                    return Tracking;
                case XR::XRCapabilityState::TemporarilyUnavailable:
                    return WaitingForSample;
                default:
                    return Unavailable;
            }
        } catch (const std::bad_alloc &) {
            return Unavailable;
        }
    }

    /** @copydoc Editor::IEditorXrPlaneCapturePreview::ArmPlaneCapture */
    Result<void> OpenXRPlayHost::ArmPlaneCapture() {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!SetupIdle())
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        if (!composition_.session.MayArmPlaneCapture())
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported));
        planesArmed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc Editor::IEditorXrPlaneCapturePreview::RevokePlaneCapture */
    void OpenXRPlayHost::RevokePlaneCapture() noexcept {
        if (std::this_thread::get_id() != lane_)
            return;
        planesArmed_ = false;
        composition_.session.RevokePlaneCapture();
    }

    Editor::EditorXrSpatialAnchorState OpenXRPlayHost::ReadSpatialAnchorState() const {
        using enum Editor::EditorXrSpatialAnchorState;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || instanceLost_)
            return Unavailable;
        if (spatialAnchorArmed_)
            return Armed;
        if (SetupIdle() && composition_.session.MayArmSpatialAnchor())
            return Ready;
        if (eventsPending_ || !input_.admitted || input_.play != Editor::EditorPlayState::Playing || !sessions_->Snapshot().admitsActions)
            return Unavailable;
        switch (composition_.session.ReadSpatialAnchorState()) {
            case XR::XRCapabilityState::Available:
                return Localized;
            case XR::XRCapabilityState::TemporarilyUnavailable:
                return Locating;
            default:
                return Unavailable;
        }
    }

    Result<void> OpenXRPlayHost::ArmSpatialAnchor() {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!SetupIdle())
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        if (!composition_.session.MayArmSpatialAnchor())
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported));
        spatialAnchorArmed_ = true;
        return Result<void>::Success();
    }

    void OpenXRPlayHost::RevokeSpatialAnchor() noexcept {
        if (std::this_thread::get_id() != lane_)
            return;
        spatialAnchorArmed_ = false;
        composition_.session.RevokeSpatialAnchorPreview();
    }

    Editor::EditorXrAnchorStorageState OpenXRPlayHost::ReadAnchorStorageState() const {
        using enum Editor::EditorXrAnchorStorageState;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || instanceLost_)
            return Unavailable;
        if (anchorStorageArmed_)
            return Armed;
        if (SetupIdle() && composition_.session.MayArmSpatialAnchorStorage())
            return Ready;
        if (!eventsPending_ && composition_.session.HasSpatialAnchorStorageConsent())
            return Granted;
        return Unavailable;
    }

    Result<void> OpenXRPlayHost::ArmAnchorStorage() {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!SetupIdle())
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        if (!composition_.session.MayArmSpatialAnchorStorage())
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported));
        anchorStorageArmed_ = true;
        return Result<void>::Success();
    }

    void OpenXRPlayHost::RevokeAnchorStorage() noexcept {
        if (std::this_thread::get_id() != lane_)
            return;
        anchorStorageArmed_ = false;
        composition_.session.RevokeSpatialAnchorStorage();
    }

    /** @copydoc Editor::IEditorXrPassthroughPreview::ReadPassthroughPreviewState */
    Editor::EditorXrPassthroughState OpenXRPlayHost::ReadPassthroughPreviewState() noexcept {
        using enum Editor::EditorXrPassthroughState;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || instanceLost_)
            return Unavailable;
        if (passthroughArmed_)
            return Armed;
        if (!passthroughScope_ && SetupIdle() && composition_.session.MayArmPassthrough())
            return Ready;
        const auto state = ReadPassthroughState();
        if (state == XR::XRCapabilityState::Denied)
            return Denied;
        if (state == XR::XRCapabilityState::Available)
            return Active;
        return scene_ && scene_->passthroughGrant ? Pending : Unavailable;
    }

    /** @copydoc Editor::IEditorXrPassthroughPreview::ArmPassthrough */
    Result<void> OpenXRPlayHost::ArmPassthrough() {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!SetupIdle() || passthroughArmed_ || passthroughScope_)
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        if (!composition_.session.MayArmPassthrough())
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported));
        if (nextPassthroughGrant_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(XR::XRErrors::CapacityExceeded));
        const auto next = XR::XRPassthroughGrant::Create(nextPassthroughGrant_ + 1);
        if (next.HasError())
            return Result<void>::Failure(next.ErrorValue());
        nextPassthroughGrant_ = next.Value().Value();
        passthroughArmed_ = next.Value();
        passthroughState_ = XR::XRCapabilityState::TemporarilyUnavailable;
        return Result<void>::Success();
    }

    /** @copydoc Editor::IEditorXrPassthroughPreview::RevokePassthrough */
    void OpenXRPlayHost::RevokePassthrough() noexcept {
        if (std::this_thread::get_id() != lane_)
            return;
        passthroughArmed_.reset();
        if (scene_)
            scene_->passthroughGrant.reset();
        passthroughState_ = XR::XRCapabilityState::Unavailable;
        if (passthroughScope_)
            native_.RevokePassthrough(*passthroughScope_);
    }

    /** @copydoc OpenXRPlayHost::MatchesPassthroughGrant */
    bool OpenXRPlayHost::MatchesPassthroughGrant(const XR::XRPassthroughScope &scope) const noexcept {
        const auto state = sessions_->Snapshot();
        return !closing_ && !eventsPending_ && scene_ && scene_->published && !scene_->retiring && scene_->session == scope.session &&
               scene_->passthroughGrant == scope.grant && input_.admitted && input_.play == Editor::EditorPlayState::Playing &&
               input_.origin == scope.world.worldOriginRevision && state.session == scope.session && state.admitsActions;
    }

    /** @copydoc OpenXRPlayHost::AuthorizeActivePassthrough */
    bool OpenXRPlayHost::AuthorizeActivePassthrough(XR::XRPassthroughScope scope) noexcept {
        using XR::OpenXRInternal::NativePassthroughState;
        if (passthroughScope_ != scope || !MatchesPassthroughGrant(scope))
            return false;
        const bool permitted = composition_.session.AllowsPassthrough(scope);
        if (passthroughScope_ != scope || !MatchesPassthroughGrant(scope))
            return false;
        if (!permitted) {
            RevokePassthrough();
            passthroughState_ = XR::XRCapabilityState::Denied;
            return false;
        }
        return native_.ReadPassthrough(scope) == NativePassthroughState::Active;
    }

    /** @copydoc OpenXRPlayHost::ReadPassthroughState */
    XR::XRCapabilityState OpenXRPlayHost::ReadPassthroughState() noexcept {
        using enum XR::XRCapabilityState;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || instanceLost_)
            return Unavailable;
        if (passthroughState_ != Available)
            return passthroughState_;
        if (passthroughScope_ && AuthorizeActivePassthrough(*passthroughScope_))
            return Available;
        return passthroughState_ == Denied ? Denied : Unavailable;
    }

    /** @copydoc OpenXRPlayHost::ProgressPassthrough */
    void OpenXRPlayHost::ProgressPassthrough() {
        const bool idle = frames_.Snapshot().phase == XR::XRFramePhase::Idle;
        if (passthroughScope_) {
            const auto scope = *passthroughScope_;
            if (AuthorizeActivePassthrough(scope) || passthroughScope_ != scope)
                return;
            if (scene_ && scene_->passthroughGrant == scope.grant)
                RevokePassthrough();
            if (idle && (native_.ReadPassthrough(scope) == XR::OpenXRInternal::NativePassthroughState::Absent ||
                         native_.StopPassthrough(scope).HasValue()))
                passthroughScope_.reset();
            return;
        }
        if (idle)
            ActivatePassthrough();
    }

    /** @copydoc OpenXRPlayHost::ActivatePassthrough */
    void OpenXRPlayHost::ActivatePassthrough() {
        const auto scene = scene_;
        if (!scene || !scene->published || scene->retiring || !scene->passthroughGrant || !input_.admitted ||
            input_.play != Editor::EditorPlayState::Playing || !sessions_->Snapshot().admitsActions)
            return;
        const auto session = scene->session;
        const auto grant = *scene->passthroughGrant;
        const auto world = composition_.session.WorldSpace(session);
        // Host admission during World lookup can revoke this exact published Scene.
        if (world.HasError() || scene_ != scene || scene->session != session || scene->retiring || scene->passthroughGrant != grant) {
            RevokePassthrough();
            return;
        }
        const XR::XRPassthroughScope scope{session, world.Value(), grant};
        if (!MatchesPassthroughGrant(scope))
            return;
        // Preserve exact scope even when native creation failed partway and left a cleanup prefix.
        passthroughScope_ = scope;
        const XR::OpenXRInternal::NativePassthroughAuthorization authorization{this,
                                                                               [](void *context, const XR::XRPassthroughScope &pending) {
            const auto &host = *static_cast<OpenXRPlayHost *>(context);
            return host.MatchesPassthroughGrant(pending) && host.composition_.session.AllowsPassthrough(pending) &&
                   host.MatchesPassthroughGrant(pending);
        }};
        const auto started = native_.StartPassthrough(scope, authorization);
        if (started.HasError()) {
            const bool stillRequested = MatchesPassthroughGrant(scope);
            RevokePassthrough();
            if (stillRequested)
                passthroughState_ = PassthroughStartFailure(started.ErrorValue());
            return;
        }
        if (!AuthorizeActivePassthrough(scope)) {
            if (scene_ == scene && scene->passthroughGrant == grant)
                RevokePassthrough();
            return;
        }
        passthroughState_ = XR::XRCapabilityState::Available;
    }

    /** @copydoc Editor::IEditorXrEyeGazePreview::ReadEyeGazeState */
    Editor::EditorXrEyeGazeState OpenXRPlayHost::ReadEyeGazeState() const noexcept {
        using enum Editor::EditorXrEyeGazeState;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || instanceLost_)
            return Unavailable;
        if (gazeArmed_)
            return Armed;
        if (SetupIdle() && composition_.session.MayArmEyeGaze())
            return Ready;
        if (composition_.session.EyeGazeAvailability() == XR::XRCapabilityState::Available)
            return ReadEyeGaze() ? Tracking : WaitingForSample;
        return Unavailable;
    }

    /** @copydoc Editor::IEditorXrEyeGazePreview::ArmEyeGaze */
    Result<void> OpenXRPlayHost::ArmEyeGaze() {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!SetupIdle())
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        if (!composition_.session.MayArmEyeGaze())
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported));
        gazeRenderingArmed_ = false;
        gazeArmed_ = true;
        return Result<void>::Success();
    }

    bool OpenXRPlayHost::CanArmEyeGazeRendering() const noexcept {
        return std::this_thread::get_id() == lane_ && !closing_ && !busy_ && SetupIdle() && composition_.session.MayArmEyeGazeRendering();
    }

    bool OpenXRPlayHost::ArmEyeGazeRendering() {
        if (!CanArmEyeGazeRendering())
            return false;
        gazeRenderingArmed_ = true;
        gazeArmed_ = true;
        return true;
    }

    /** @copydoc Editor::IEditorXrEyeGazePreview::RevokeEyeGaze */
    void OpenXRPlayHost::RevokeEyeGaze() noexcept {
        if (std::this_thread::get_id() != lane_)
            return;
        gazeArmed_ = false;
        gazeRenderingArmed_ = false;
        if (scene_)
            scene_->gazeRenderingConsent = false;
        composition_.session.RevokeEyeGaze();
    }

    /** @copydoc Editor::IEditorXrSetup::RequestSetupTest */
    Result<Editor::EditorXrSetupTestId> OpenXRPlayHost::RequestSetupTest() {
        using Request = Result<Editor::EditorXrSetupTestId>;
        if (auto lane = Lane(); lane.HasError())
            return Request::Failure(std::move(lane).ErrorValue());
        if (instanceLost_)
            return Request::Failure(MakeError(XR::XRErrors::CapabilityStale));
        if (!SetupIdle() || queuedSetupTest_.value)
            return Request::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        if (lastSetupTest_ == std::numeric_limits<std::uint64_t>::max())
            return Request::Failure(MakeError(XR::XRErrors::CapacityExceeded));
        queuedSetupTest_ = {++lastSetupTest_};
        setup_.test = Editor::EditorXrSetupTestState::Queued;
        setup_.issue = setupCleanupPending_ ? Editor::EditorXrSetupIssue::Cleanup : Editor::EditorXrSetupIssue::None;
        return Request::Success(queuedSetupTest_);
    }

    /** @copydoc Editor::IEditorXrSetup::CancelSetupTest */
    void OpenXRPlayHost::CancelSetupTest(const Editor::EditorXrSetupTestId request) noexcept {
        if (std::this_thread::get_id() != lane_ || request.value == 0 || queuedSetupTest_ != request)
            return;
        queuedSetupTest_ = {};
        setup_.test = Editor::EditorXrSetupTestState::Cancelled;
    }

    void OpenXRPlayHost::RequestSetupRefresh() noexcept {
        if (std::this_thread::get_id() == lane_ && !closing_ && setupPreflight_)
            setupRefreshRequested_ = true;
    }

    /** @copydoc OpenXRPlayHost::PumpSetupTest */
    void OpenXRPlayHost::PumpSetupTest() {
        if (std::this_thread::get_id() != lane_ || closing_)
            return;
        if (!queuedSetupTest_.value) {
            if (busy_ || !std::exchange(setupRefreshRequested_, false) || instanceLost_ || !setupPreflight_)
                return;
            const Operation operation{*this};
            const auto current =
                composition_.session.ValidateDeviceQualification(*setupPreflight_, composition_.frontend.Capabilities().support);
            if (current.HasError()) {
                setup_.test = Editor::EditorXrSetupTestState::Failed;
                setup_.issue = SetupIssue(current.ErrorValue());
                setupPreflight_.reset();
                composition_.session.RevokePackagedPreflight();
            }
            return;
        }
        queuedSetupTest_ = {};
        setup_.test = Editor::EditorXrSetupTestState::Failed;
        if (!SetupIdle()) {
            setup_.issue = Editor::EditorXrSetupIssue::Busy;
            return;
        }
        // Only a started replacement invalidates the previous qualification.
        setupRefreshRequested_ = false;
        setupPreflight_.reset();
        const Operation operation{*this};
        if (setupCleanupPending_) {
            const auto cleaned = composition_.session.RetirePreparation();
            if (cleaned.HasError()) {
                setup_.issue = Editor::EditorXrSetupIssue::Cleanup;
                return;
            }
            setupCleanupPending_ = false;
        }
        auto result = composition_.session.RunPackagedPreflight(*composition_.bindings, composition_.frontend.Capabilities().support);
        if (result.HasError()) {
            setup_.issue = SetupIssue(result.ErrorValue());
            setupCleanupPending_ = setup_.issue == Editor::EditorXrSetupIssue::Cleanup;
            return;
        }
        setupPreflight_.emplace(std::move(result).Value());
        setup_.test = Editor::EditorXrSetupTestState::Passed;
        setup_.issue = Editor::EditorXrSetupIssue::None;
    }

    /** @copydoc OpenXRPlayHost::ReadDeviceQualificationPreflight */
    Result<OpenXRPackagedPreflight> OpenXRPlayHost::ReadDeviceQualificationPreflight() const {
        using Outcome = Result<OpenXRPackagedPreflight>;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        if (!setupPreflight_)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        if (const auto allowed =
                composition_.session.ValidateDeviceQualification(*setupPreflight_, composition_.frontend.Capabilities().support);
            allowed.HasError())
            return Outcome::Failure(allowed.ErrorValue());
        return Outcome::Success(*setupPreflight_);
    }
}  // namespace Horo::Application::Internal
