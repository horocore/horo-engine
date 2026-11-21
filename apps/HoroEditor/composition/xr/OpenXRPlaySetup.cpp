#include "Horo/XR/XRSessionErrors.h"
#include "OpenXRPlayHost.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
        if (!LightingCutoff(scope))
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        if (scene_->nativeLighting)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        auto projection = XRLightProjection::Create(scope, policy);
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        const Operation operation{*this};
        if (auto native = native_.StartLightEstimation(scope, limits); native.HasError())
            return native;
        const bool admitted = !pendingRevoke_ && LightingCutoff(scope).has_value();
        scene_->nativeLighting.emplace(scope, std::move(projection).Value(), admitted);
        if (!admitted) {
            if (auto retired = native_.StopLightEstimation(scope); retired.HasError())
                return retired;
            scene_->nativeLighting.reset();
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::StopNativeLighting */
    Result<void> OpenXRPlayHost::StopNativeLighting() {
        if (const auto lane = Lane(); lane.HasError())
            return lane;
        if (!scene_ || !scene_->nativeLighting)
            return Result<void>::Success();
        auto &light = *scene_->nativeLighting;
        light.active = false;
        if (frames_.Snapshot().phase != XR::XRFramePhase::Idle)
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        const Operation operation{*this};
        if (auto retired = native_.StopLightEstimation(light.scope); retired.HasError())
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
        const auto actions = tracking.Actions();
        const auto count = std::min(actions.size(), out.actions.size());
        out.actionCount = static_cast<std::uint16_t>(actions.size());
        out.activeActionCount = static_cast<std::uint16_t>(std::ranges::count(actions, true, &XR::XRTrackedActionRecord::active));
        out.omittedActionCount = static_cast<std::uint16_t>(actions.size() - count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto &action = actions[index];
            out.actions[index] = {action.bindingIndex, action.device.slot.generation, action.kind, action.active};
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
            report.issue = Editor::EditorXrSetupIssue::Stale;
            return report;
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

        const auto state = sessions_.Snapshot();
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
        const auto plan = sessions_.AcceptedPlan();
        if (!plan || !scene_->projection || scene_->session != state.session ||
            scene_->projection->capabilities.Revision() != state.capabilityRevision || plan->Revision() != state.capabilityRevision) {
            report.status = Stale;
            return report;
        }
        report.runtimeGeneration = state.session.system.runtime.Value();
        report.systemGeneration = state.session.system.slot.generation;
        report.sessionGeneration = state.session.slot.generation;
        report.capabilityRevision = state.capabilityRevision.Value();
        report.focused = state.admitsActions && !eventsPending_;
        CopyInspectionCapabilities(report, scene_->projection->capabilities, *plan, composition_.session.EyeGazeAvailability());

        if (eventsPending_)
            report.status = EventsPending;
        else if (!state.admitsActions)
            report.status = Unfocused;
        else if (const auto *tracking = tracking_.Current(); tracking && tracking->Session() == state.session &&
                                                             tracking->WorldOriginRevision() == input_.origin && input_.admitted &&
                                                             input_.play == Editor::EditorPlayState::Playing) {
            CopyOpenXRInspectionTracking(report, *tracking);
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
        const auto session = sessions_.Snapshot();
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
            if (preview.mayNeedStop && sessions_.Snapshot().session == preview.target.session)
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
            if (sessions_.Snapshot().session == preview.target.session)
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
            input_.play != Editor::EditorPlayState::Playing || !sessions_.Snapshot().admitsActions)
            return std::nullopt;
        const auto *snapshot = tracking_.Current();
        return snapshot ? composition_.session.ReadEyeGaze(*snapshot) : std::nullopt;
    }

    /** @copydoc OpenXRPlayHost::ReadPlaneObservations */
    Result<XR::XRPlaneSnapshot> OpenXRPlayHost::ReadPlaneObservations() const {
        using Outcome = Result<XR::XRPlaneSnapshot>;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || eventsPending_ || !input_.admitted ||
            input_.play != Editor::EditorPlayState::Playing || !sessions_.Snapshot().admitsActions)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        const auto *tracking = tracking_.Current();
        return tracking ? composition_.session.ReadPlaneObservations(*tracking)
                        : Outcome::Failure(MakeError(XR::XRErrors::TrackingSnapshotStale));
    }

    /** @copydoc OpenXRPlayHost::QueryPlaneHits */
    Result<XR::XRSpatialHits> OpenXRPlayHost::QueryPlaneHits(const XR::XRSpatialHitRequest &request) const {
        auto source = ReadPlaneObservations();
        if (source.HasError())
            return Result<XR::XRSpatialHits>::Failure(source.ErrorValue());
        return XR::QueryXRPlaneHits(source.Value(), request);
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
        if (eventsPending_ || !input_.admitted || input_.play != Editor::EditorPlayState::Playing || !sessions_.Snapshot().admitsActions)
            return Unavailable;
        switch (composition_.session.PlaneCaptureAvailability()) {
            case XR::XRCapabilityState::Available:
                return Tracking;
            case XR::XRCapabilityState::TemporarilyUnavailable:
                return WaitingForSample;
            default:
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

    /** @copydoc Editor::IEditorXrPassthroughPreview::ReadPassthroughPreviewState */
    Editor::EditorXrPassthroughState OpenXRPlayHost::ReadPassthroughPreviewState() const noexcept {
        using enum Editor::EditorXrPassthroughState;
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || instanceLost_)
            return Unavailable;
        if (passthroughArmed_)
            return Armed;
        if (!passthroughScope_ && SetupIdle() && composition_.session.MayArmPassthrough())
            return Ready;
        if (passthroughState_ == XR::XRCapabilityState::Denied)
            return Denied;
        if (ReadPassthroughState() == XR::XRCapabilityState::Available)
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
        const auto state = sessions_.Snapshot();
        return !closing_ && !eventsPending_ && scene_ && scene_->published && !scene_->retiring && scene_->session == scope.session &&
               scene_->passthroughGrant == scope.grant && input_.admitted && input_.play == Editor::EditorPlayState::Playing &&
               input_.origin == scope.world.worldOriginRevision && state.session == scope.session && state.admitsActions;
    }

    /** @copydoc OpenXRPlayHost::HasPassthrough */
    bool OpenXRPlayHost::HasPassthrough(XR::XRPassthroughScope scope) const noexcept {
        return MatchesPassthroughGrant(scope) && native_.ReadPassthrough(scope) == XR::OpenXRInternal::NativePassthroughState::Active &&
               composition_.session.AllowsPassthrough(scope) && MatchesPassthroughGrant(scope);
    }

    /** @copydoc OpenXRPlayHost::ReadPassthroughState */
    XR::XRCapabilityState OpenXRPlayHost::ReadPassthroughState() const noexcept {
        if (std::this_thread::get_id() != lane_ || closing_ || busy_ || instanceLost_)
            return XR::XRCapabilityState::Unavailable;
        if (passthroughState_ == XR::XRCapabilityState::Available && (!passthroughScope_ || !HasPassthrough(*passthroughScope_)))
            return XR::XRCapabilityState::Unavailable;
        return passthroughState_;
    }

    /** @copydoc OpenXRPlayHost::ProgressPassthrough */
    void OpenXRPlayHost::ProgressPassthrough() {
        const bool idle = frames_.Snapshot().phase == XR::XRFramePhase::Idle;
        if (passthroughScope_) {
            const auto scope = *passthroughScope_;
            if (HasPassthrough(scope))
                return;
            if (scene_ && scene_->passthroughGrant) {
                const bool permitted = composition_.session.AllowsPassthrough(scope);
                RevokePassthrough();
                if (!permitted)
                    passthroughState_ = XR::XRCapabilityState::Denied;
            }
            if (idle && (native_.ReadPassthrough(scope) == XR::OpenXRInternal::NativePassthroughState::Absent ||
                         native_.StopPassthrough(scope).HasValue()))
                passthroughScope_.reset();
            return;
        }
        const auto scene = scene_;
        if (!idle || !scene || !scene->published || scene->retiring || !scene->passthroughGrant || !input_.admitted ||
            input_.play != Editor::EditorPlayState::Playing || !sessions_.Snapshot().admitsActions)
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
        if (!composition_.session.AllowsPassthrough(scope)) {
            RevokePassthrough();
            passthroughState_ = XR::XRCapabilityState::Denied;
            return;
        }
        // Platform permission is a callback. Its result cannot restore a Play grant revoked during that call.
        if (!MatchesPassthroughGrant(scope))
            return;
        // Preserve exact scope even when native creation failed partway and left a cleanup prefix.
        passthroughScope_ = scope;
        const auto started = native_.StartPassthrough(scope);
        if (started.HasError()) {
            RevokePassthrough();
            passthroughState_ = PassthroughStartFailure(started.ErrorValue());
            return;
        }
        if (!HasPassthrough(scope)) {
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
        gazeArmed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc Editor::IEditorXrEyeGazePreview::RevokeEyeGaze */
    void OpenXRPlayHost::RevokeEyeGaze() noexcept {
        if (std::this_thread::get_id() != lane_)
            return;
        gazeArmed_ = false;
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
        setupPreflight_.reset();
        queuedSetupTest_ = {++lastSetupTest_};
        setup_.test = Editor::EditorXrSetupTestState::Queued;
        return Request::Success(queuedSetupTest_);
    }

    /** @copydoc Editor::IEditorXrSetup::CancelSetupTest */
    void OpenXRPlayHost::CancelSetupTest(const Editor::EditorXrSetupTestId request) noexcept {
        if (std::this_thread::get_id() != lane_ || request.value == 0 || queuedSetupTest_ != request)
            return;
        queuedSetupTest_ = {};
        setup_.test = Editor::EditorXrSetupTestState::Cancelled;
    }

    /** @copydoc OpenXRPlayHost::PumpSetupTest */
    void OpenXRPlayHost::PumpSetupTest() {
        if (std::this_thread::get_id() != lane_ || !queuedSetupTest_.value)
            return;
        queuedSetupTest_ = {};
        setup_.test = Editor::EditorXrSetupTestState::Failed;
        if (!SetupIdle()) {
            setup_.issue = Editor::EditorXrSetupIssue::Busy;
            return;
        }
        const Operation operation{*this};
        if (setupCleanupPending_) {
            const auto cleaned = composition_.session.RetirePreparation();
            if (cleaned.HasError()) {
                setup_.issue = Editor::EditorXrSetupIssue::Cleanup;
                return;
            }
            setupCleanupPending_ = false;
        }
        auto result = composition_.session.RunPackagedPreflight(*composition_.bindings);
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
        if (const auto allowed = composition_.session.ValidateDeviceQualification(*setupPreflight_); allowed.HasError())
            return Outcome::Failure(allowed.ErrorValue());
        return Outcome::Success(*setupPreflight_);
    }
}  // namespace Horo::Application::Internal
