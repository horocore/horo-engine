#include "OpenXRPlayHost.h"

#include "Horo/Foundation/Assertions.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Physics/CharacterWorld.h"
#include "OpenXRPlayFrameState.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    /** @copydoc OpenXRPlayHost::OpenXRPlayHost */
    OpenXRPlayHost::OpenXRPlayHost(const ConstructionKey &, OpenXRPlayHostComposition composition, OpenXRApplicationPreparation preparation)
        : composition_{composition.session,
                       composition.viewport,
                       composition.frontend,
                       std::move(composition.bindings),
                       std::move(composition.dispatch),
                       composition.fixedStep,
                       composition.trackingRoot,
                       composition.lighting,
                       composition.metrics,
                       composition.captureClock},
          native_(preparation.native), discoveryAttempt_(preparation.preflight.Attempt()),
          locomotionComfort_(preparation.settings.LocomotionComfort()), getProc_(preparation.getProc), clock_(native_) {
        input_.router = &composition.router;
        const auto version = preparation.preflight.LoaderApiVersion();
        setup_ = {.loaderMajor = version.major,
                  .loaderMinor = version.minor,
                  .loaderPatch = version.patch,
                  .requestedProfile = preparation.settings.Profile(),
                  .rendererReady = true,
                  .issue = Editor::EditorXrSetupIssue::None};
    }

    /** @copydoc OpenXRPlayHost::Create */
    Result<std::unique_ptr<OpenXRPlayHost>> OpenXRPlayHost::Create(OpenXRPlayHostComposition composition) {
        if (!composition.bindings || !composition.dispatch.IsValid() || composition.fixedStep.ToNanoseconds() <= 0 ||
            !composition.trackingRoot.IsValid())
            return Result<std::unique_ptr<OpenXRPlayHost>>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (const auto prepared = composition.session.PrepareSession(); prepared.HasError())
            return Result<std::unique_ptr<OpenXRPlayHost>>::Failure(prepared.ErrorValue());
        const auto facts = composition.session.BorrowPreparation();
        if (facts.HasError())
            return Result<std::unique_ptr<OpenXRPlayHost>>::Failure(facts.ErrorValue());
        auto host = std::make_unique<OpenXRPlayHost>(ConstructionKey{}, std::move(composition), facts.Value());
        if (auto clock = host->clock_.PrepareScheduler(host->composition_.dispatch, host->composition_.fixedStep); clock.HasError())
            return Result<std::unique_ptr<OpenXRPlayHost>>::Failure(clock.ErrorValue());
        return Result<std::unique_ptr<OpenXRPlayHost>>::Success(std::move(host));
    }

    OpenXRPlayHost::~OpenXRPlayHost() {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_ && !busy_ && !scene_ && !render_ && !events_,
                           "XR Play owner requires explicit physical Scene/frame/native retirement before destruction");
    }

    /** @copydoc OpenXRPlayHost::Close */
    Result<bool> OpenXRPlayHost::Close() {
        if (std::this_thread::get_id() != lane_ || busy_)
            return Result<bool>::Failure(MakeError(XRErrors::OperationIncompatible));
        CancelSetupTest(queuedSetupTest_);
        closing_ = true;
        // A failed native Begin/End cannot be retried as a successful zero-layer frame. The same host
        // revokes admission and retains all image/queue prefixes until its existing Lost cleanup proves quiescence.
        if (render_ && render_->compositor.RetainedFault())
            RevokeEventAdmission();
        if (scene_)
            scene_->Retire(*this);
        RevokeInput();
        const Operation operation{*this};
        if (scene_)
            return SceneState::ProgressRetirement(*this);
        if (auto retired = composition_.session.RetirePreparation(); retired.HasError())
            return Result<bool>::Failure(retired.ErrorValue());
        return Result<bool>::Success(true);
    }

    /** @copydoc OpenXRPlayHost::SetCaptureClock */
    void OpenXRPlayHost::SetCaptureClock(const OpenXRInputCaptureClock captureClock) noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_ && !busy_, "XR capture clock binding requires the idle owner lane");
        composition_.session.RevokeInputCaptureTime();
        composition_.captureClock = captureClock;
    }

    /** @copydoc OpenXRPlayHost::Lane */
    Result<void> OpenXRPlayHost::Lane() const {
        if (std::this_thread::get_id() != lane_ || busy_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (closing_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::FinishOperation */
    void OpenXRPlayHost::FinishOperation() noexcept {
        busy_ = false;
        if (pendingRevoke_) {
            pendingRevoke_ = false;
            composition_.session.RevokeInput();
        }
        if (pendingDetach_) {
            pendingDetach_ = false;
            input_.port.reset();
            input_.geometry.reset();
        }
    }

    /** @copydoc OpenXRPlayHost::AttachInput */
    Result<void> OpenXRPlayHost::AttachInput(const Input::InputActionCollection &input, const Editor::EditorXrSceneSourcePort &geometry) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!input_.router || input_.port)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        input_.port.emplace(input);
        input_.geometry.emplace(geometry);
        // The actual Scene participant reserves native actions/Input against this port and its exact origin.
        // Merely entering a workspace cannot manufacture a Ready session or a semantic registration.
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::SetPlayState */
    Result<void> OpenXRPlayHost::SetPlayState(const Editor::EditorPlayState state) {
        using enum Horo::Editor::EditorPlayState;
        if (auto lane = Lane(); lane.HasError()) {
            return lane;
        }
        if (state > Failed) {
            RevokeSource();
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        }
        input_.play = state;
        if (state == Paused) {
            tracking_.Revoke();
            input_.neutralPending = true;
            composition_.session.RequestHapticCancellation();
            RevokeSensitiveObservations();
        }
        if (state == Idle || state == Stopping || state == Failed)
            RevokeSource();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::RevokeSource */
    void OpenXRPlayHost::RevokeSource() noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_, "XR Play revocation requires its owner lane");
        input_.admitted = false;
        CancelAuthoredLocomotion();
        tracking_.Revoke();
        RevokeSensitiveObservations();
        composition_.session.RequestHapticCancellation();
        if (busy_) {
            if (scene_ && !scene_->activated)
                scene_->preparationCancellation.RequestCancellation();
            pendingRevoke_ = true;
            return;
        }
        composition_.session.RevokeInput();
    }

    /** @copydoc OpenXRPlayHost::RevokeSensitiveObservations */
    void OpenXRPlayHost::RevokeSensitiveObservations() noexcept {
        RevokeEyeGaze();
        RevokePlaneCapture();
        spatialAnchorArmed_ = false;
        anchorStorageArmed_ = false;
        composition_.session.RevokeSpatialAnchor();
        if (scene_ && scene_->nativeLighting)
            scene_->nativeLighting->active = false;
        RevokePassthrough();
    }

    /** @copydoc OpenXRPlayHost::RevokeInput */
    void OpenXRPlayHost::RevokeInput() noexcept {
        RevokeSource();
        if (busy_) {
            pendingDetach_ = true;
            return;
        }
        input_.port.reset();
        input_.geometry.reset();
    }

    /** @copydoc OpenXRPlayHost::DetachInput */
    Result<void> OpenXRPlayHost::DetachInput() {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        RevokeInput();
        return busy_ ? Result<void>::Failure(MakeError(XRErrors::OperationIncompatible)) : Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::PollEvents */
    Result<void> OpenXRPlayHost::PollEvents() {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        const Operation operation{*this};
        if (sessions_->Snapshot().state == XRSessionState::Lost && scene_)
            scene_->Retire(*this);
        if (scene_ && scene_->retiring) {
            const auto retired = SceneState::ProgressRetirement(*this);
            if (retired.HasError())
                return Result<void>::Failure(retired.ErrorValue());
            return Result<void>::Success();
        }
        if (eventsBound_) {
            const bool wantsPlay =
                input_.admitted && (input_.play == Editor::EditorPlayState::Starting || input_.play == Editor::EditorPlayState::Playing ||
                                    input_.play == Editor::EditorPlayState::Paused);
            const auto observed = events_->Poll(wantsPlay);
            if (observed.HasError()) {
                RevokeEventAdmission();
                return Result<void>::Failure(observed.ErrorValue());
            }
            const auto applied = ApplyEventObservation(observed.Value());
            if (applied.HasError())
                return Result<void>::Failure(applied.ErrorValue());
            if (!applied.Value())
                return Result<void>::Success();
        }
        ProgressPassthrough();
        ProgressNativeLighting();
        if (auto tail = ProgressTail(); tail.HasError()) {
            RevokeEventAdmission();
            return tail;
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::ApplyEventObservation */
    Result<bool> OpenXRPlayHost::ApplyEventObservation(const NativeSessionEventObservation &observed) {
        const bool inputSuspended = observed.actionsInterrupted || (!eventsPending_ && observed.morePending);
        eventsPending_ = observed.morePending;
        if (observed.loss == NativeSessionEventLoss::Instance) {
            instanceLost_ = true;
            composition_.session.RevokePackagedPreflight();
        }
        if (observed.loss != NativeSessionEventLoss::None || observed.exitRequested) {
            if (scene_)
                scene_->Retire(*this);
            return Result<bool>::Success(false);
        }
        const auto state = sessions_->Snapshot();
        if (inputSuspended) {
            tracking_.Revoke();
            input_.neutralPending = true;
        }
        if (observed.actionsInterrupted)
            RevokeSensitiveObservations();
        const bool invalidateOutput = inputSuspended || observed.bindingsChanged;
        if (invalidateOutput)
            composition_.session.RequestHapticCancellation();
        if (invalidateOutput || state.admitsActions) {
            if (auto stopped = composition_.session.ProgressHapticCancellation(state.session); stopped.HasError()) {
                RevokeEventAdmission();
                return Result<bool>::Failure(stopped.ErrorValue());
            }
        }
        if (observed.bindingsChanged) {
            tracking_.Revoke();
            if (const auto refreshed = composition_.session.RefreshInputProfiles(state.session); refreshed.HasError()) {
                if (RetireIfInputProfileChanged(refreshed.ErrorValue()))
                    return Result<bool>::Success(false);
                RevokeEventAdmission();
                return Result<bool>::Failure(refreshed.ErrorValue());
            }
        }
        if (const auto &change = observed.referenceChange; change && scene_ && scene_->projection && change->session == scene_->session) {
            if (change->kind == XRSpaceKind::Stage) {
                const auto decision = scene_->projection->plan.Decision(XRCapability::StageSpace).state;
                if (decision != XRFeatureDecisionState::Required && decision != XRFeatureDecisionState::OptionalEnabled)
                    return Result<bool>::Success(false);
            }
            if (!lastReferenceChange_ || lastReferenceChange_->session != change->session ||
                change->effectiveTime > lastReferenceChange_->effectiveTime)
                lastReferenceChange_ = change;
            // Retirement never waits on reference time. Only new Scene admission borrows that fence.
            if (!scene_->retiring)
                scene_->Retire(*this);
        }
        return Result<bool>::Success(!observed.morePending);
    }

    /** @copydoc OpenXRPlayHost::RevokeEventAdmission */
    void OpenXRPlayHost::RevokeEventAdmission() noexcept {
        if (const auto state = sessions_->Snapshot(); state.session.IsValid()) {
            // The copied publication is current on this serialized lane. SessionLost is legal from
            // every published state and only revokes admission; it never destroys native children.
            const auto lost = sessions_->ApplyEvent(state.session, XRSessionEvent::SessionLost);
            HORO_INVARIANT_MSG(lost.HasValue(), "Current XR event failure must revoke canonical frame admission");
        }
        if (scene_)
            scene_->Retire(*this);
        else
            RevokeSource();
    }

    /** @copydoc OpenXRPlayHost::RetireIfInputProfileChanged */
    bool OpenXRPlayHost::RetireIfInputProfileChanged(const Error &error) {
        if (!ErrorChainContains(error, XRErrors::ActionProfileChanged.domain, XRErrors::ActionProfileChanged.code))
            return false;
        if (scene_)
            scene_->Retire(*this);
        else
            RevokeSource();
        LOG_WARN("editor.xr", "Controller profile changed. Start a new XR Play session to use the current profile.");
        return true;
    }

    /** @copydoc OpenXRPlayHost::AppendInput */
    Result<void> OpenXRPlayHost::AppendInput(const Runtime::FrameContext &context, const Input::InputCollectionFrame &frame) {
        if (!input_.coordinator)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto placement = scene_ && scene_->locomotion ? scene_->locomotion->worldPlacement : std::nullopt;
        auto sampled = composition_.session.AppendInput(*sessions_, *input_.coordinator, tracking_, clock_, context, input_.origin, frame,
                                                        {placement ? &*placement : nullptr, composition_.captureClock});
        input_.sampledWorldPlacement = sampled.HasValue() ? placement : std::nullopt;
        return sampled;
    }

    /** @copydoc OpenXRPlayHost::ReadReferences */
    Result<XRReferenceSpaceSnapshot> OpenXRPlayHost::ReadReferences(XRSessionId session) const {
        using Outcome = Result<XRReferenceSpaceSnapshot>;
        if (auto lane = Lane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        const auto state = sessions_->Snapshot();
        if (auto valid = ValidateXRSession(session, state.session); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (state.state == XRSessionState::Lost || !scene_ || !scene_->published || scene_->retiring)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (eventsPending_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (scene_->geometry && scene_->geometry->mounted) {
            if (const auto source = scene_->geometry->ValidateSource(); source.HasError())
                return Outcome::Failure(source.ErrorValue());
        }
        return Outcome::Success(scene_->references);
    }

    /** @copydoc OpenXRPlayHost::ReadReferenceChange */
    std::optional<XRReferenceSpaceChange> OpenXRPlayHost::ReadReferenceChange(XRSessionId session) const noexcept {
        if (std::this_thread::get_id() != lane_ || busy_ || closing_ || !lastReferenceChange_ || lastReferenceChange_->session != session)
            return std::nullopt;
        const auto active = sessions_->Snapshot().session;
        return active.IsValid() && active != session ? std::nullopt : lastReferenceChange_;
    }

    /** @copydoc OpenXRPlayHost::ReadHandGestures */
    XRHandGestureCandidates OpenXRPlayHost::ReadHandGestures() const noexcept {
        if (std::this_thread::get_id() != lane_ || busy_ || closing_ || eventsPending_ || !input_.admitted ||
            input_.play != Editor::EditorPlayState::Playing || !sessions_->Snapshot().admitsActions)
            return {};
        const auto *snapshot = tracking_.Current();
        return snapshot ? composition_.session.ReadHandGestures(*snapshot) : XRHandGestureCandidates{};
    }

    /** @copydoc OpenXRPlayHost::HasCurrentAssignedInput */
    bool OpenXRPlayHost::HasCurrentAssignedInput() const {
        if (!input_.router || !input_.port || !input_.port->AssignedPlayer())
            return false;
        auto port = *input_.port;
        const auto refresh = input_.router->RefreshPlayerActionCollection(port);
        return refresh.HasValue() && !refresh.Value();
    }

    /** @copydoc OpenXRPlayHost::HasActiveInteractionScene */
    bool OpenXRPlayHost::HasActiveInteractionScene(const XRTrackingSnapshot &snapshot) const noexcept {
        return scene_ && scene_->published && !scene_->retiring && !eventsPending_ && input_.admitted &&
               input_.play == Editor::EditorPlayState::Playing && snapshot.Focus() == XRTrackingFocusState::Focused &&
               snapshot.Visibility() == XRTrackingVisibilityState::Visible && snapshot.State() == XRTrackingState::Active;
    }

    /** @copydoc OpenXRPlayHost::MatchesInteractionPublication */
    bool OpenXRPlayHost::MatchesInteractionPublication(const XRTrackingSnapshot &snapshot, const XRSessionId &session) const noexcept {
        return scene_->session == session && snapshot.Session() == session && snapshot.WorldOriginRevision() == input_.origin &&
               input_.coordinator && input_.coordinator->Current() && input_.coordinator->Revision() == snapshot.BindingRevision() &&
               (!scene_->geometry || scene_->geometry->IsMountedSourceCurrent());
    }

    /** @copydoc OpenXRPlayHost::CurrentInteractionTracking */
    Result<const XRTrackingSnapshot *> OpenXRPlayHost::CurrentInteractionTracking(const Input::PlayerId player) const {
        using Outcome = Result<const XRTrackingSnapshot *>;
        if (const auto lane = Lane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        if (!input_.port || input_.port->AssignedPlayer() != player)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const auto assignment = input_.port->ReadPlayerAssignmentRevision();
        const auto *snapshot = tracking_.Current();
        const auto state = sessions_->Snapshot();
        if (!snapshot || !assignment || !state.admitsActions || !HasActiveInteractionScene(*snapshot))
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (assignment != input_.interactionAssignment || !MatchesInteractionPublication(*snapshot, state.session))
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        return Outcome::Success(snapshot);
    }

    /** @copydoc OpenXRPlayHost::CurrentInteractionTracking(const OpenXRPlayInteractionScope &) const */
    Result<const XRTrackingSnapshot *> OpenXRPlayHost::CurrentInteractionTracking(const OpenXRPlayInteractionScope &scope) const {
        using Outcome = Result<const XRTrackingSnapshot *>;
        const auto current = CurrentInteractionTracking(scope.player);
        if (current.HasError())
            return current;
        if (scope.scene != scene_->scene)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (scope.cutoff != DescribeXRInteractionCutoff(*current.Value()) || scope.assignmentRevision != input_.interactionAssignment)
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        return current;
    }

    /** @copydoc OpenXRPlayListeningScope::SharesLineage */
    bool OpenXRPlayListeningScope::SharesLineage(const OpenXRPlayListeningScope &other) const noexcept {
        const auto &a = interaction;
        const auto &b = other.interaction;
        return a.scene == b.scene && a.player == b.player && a.assignmentRevision == b.assignmentRevision &&
               a.cutoff.session == b.cutoff.session && a.cutoff.origin == b.cutoff.origin &&
               a.cutoff.bindingRevision == b.cutoff.bindingRevision && reference == other.reference && world == other.world &&
               capabilities == other.capabilities && structuralRevision == other.structuralRevision;
    }

    /** @copydoc OpenXRPlayHost::ReadListeningScope */
    std::optional<OpenXRPlayListeningScope> OpenXRPlayHost::ReadListeningScope(const Input::PlayerId player) const {
        const auto tracking = CurrentInteractionTracking(player);
        if (tracking.HasError() || !scene_->geometry)
            return std::nullopt;
        const auto revision = scene_->geometry->sourceScene.StructuralRevision();
        if (revision == 0)
            return std::nullopt;
        const auto &snapshot = *tracking.Value();
        const auto world = composition_.session.WorldSpace(snapshot.Session());
        if (world.HasError() || world.Value().worldOriginRevision != snapshot.WorldOriginRevision())
            return std::nullopt;
        return OpenXRPlayListeningScope{{DescribeXRInteractionCutoff(snapshot), scene_->scene, player, *input_.interactionAssignment},
                                        scene_->references.local.id,
                                        world.Value().id,
                                        sessions_->Snapshot().capabilityRevision,
                                        revision};
    }

    /** @copydoc OpenXRPlayHost::ReadInteractionSources */
    Result<OpenXRPlayInteractionSources> OpenXRPlayHost::ReadInteractionSources(const Input::PlayerId player) const {
        using Outcome = Result<OpenXRPlayInteractionSources>;
        const auto current = CurrentInteractionTracking(player);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        const auto &snapshot = *current.Value();
        OpenXRPlayInteractionSources sources;
        sources.scope_ = {DescribeXRInteractionCutoff(snapshot), scene_->scene, player, *input_.interactionAssignment};
        sources.captureTime_ = composition_.session.ReadInputCaptureTime(snapshot);
        using enum XRTrackedDeviceCapability;
        for (const auto &device : snapshot.Devices()) {
            const auto &capabilities = device.capabilities;
            if (device.state == XRTrackedDeviceState::Tracked &&
                (capabilities.Has(AimPose) || capabilities.Has(GripPose) || capabilities.Has(HandJoints)))
                sources.devices_[sources.count_++] = device;
        }
        return Outcome::Success(std::move(sources));
    }

    /** @copydoc OpenXRPlayHost::ReadLocomotionEvidence */
    Result<OpenXRPlayLocomotionEvidence> OpenXRPlayHost::ReadLocomotionEvidence(const OpenXRPlayInteractionScope &scope,
                                                                                const Runtime::FixedStepContext &step) const {
        using Outcome = Result<OpenXRPlayLocomotionEvidence>;
        const auto current = CurrentInteractionTracking(scope);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        Runtime::RuntimeDispatchFacts facts;
        if (step.dispatchEvidence.Read(composition_.dispatch, Runtime::RuntimePhase::FixedUpdate, facts) !=
                Runtime::RuntimeDispatchStatus::Valid ||
            facts.fixedTick != step.simulationTick || facts.fixedAttempt != step.attemptNumber || facts.frame != step.frameNumber ||
            facts.fixedDuration != step.fixedDelta)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const auto assignment = input_.port->ReadPlayerAssignmentRevision();
        if (step.cancellation.IsCancellationRequested() || !assignment)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        return Outcome::Success({scope, facts.fixedTick, facts.fixedAttempt, *current.Value(), locomotionComfort_, *assignment});
    }

    /** @copydoc OpenXRPlayHost::AuthoredLocomotion */
    const OpenXRLocomotionBinding *OpenXRPlayHost::AuthoredLocomotion() const noexcept {
        return composition_.bindings && composition_.bindings->locomotion ? &*composition_.bindings->locomotion : nullptr;
    }

    /** @copydoc OpenXRPlayHost::CancelAuthoredLocomotion */
    void OpenXRPlayHost::CancelAuthoredLocomotion() noexcept {
        input_.comfortHint.reset();
        if (scene_ && scene_->locomotion)
            scene_->locomotion->pending.reset();
        if (authoredCharacter_) {
            authoredCharacter_->authority.Revoke();
            authoredCharacter_.reset();
        }
    }

    namespace {
        float YawAroundWorldUp(const Math::Quaternion orientation) noexcept {
            const auto forward = orientation.Rotate({0.0F, 0.0F, -1.0F});
            return std::atan2(-forward.x, -forward.z);
        }

        XRLocomotionProfile MakeAuthoredProfile(const OpenXRLocomotionBinding &route, XRLocomotionComfortSettings comfort) {
            XRLocomotionProfile profile{.comfort = comfort};
            if (const auto *stick = std::get_if<OpenXRStickMoveBinding>(&route.translation)) {
                profile.translation = XRSmoothMove{route.speedMetersPerSecond, stick->deadZone};
            } else if (const auto *swing = std::get_if<OpenXRArmSwingBinding>(&route.translation)) {
                profile.translation = XRArmSwingMove{route.speedMetersPerSecond, swing->gain};
                profile.maximumTrackingGapNanoseconds = swing->maximumTrackingGapNanoseconds;
            } else if (const auto *room = std::get_if<OpenXRRoomScaleBinding>(&route.translation)) {
                profile.translation = XRRoomScaleMove{route.speedMetersPerSecond, room->maximumSampleDistanceMeters};
                profile.maximumTrackingGapNanoseconds = room->maximumTrackingGapNanoseconds;
            } else {
                profile.translation = XRTeleportMove{};
            }
            if (const auto &turn = route.turn)
                profile.turn.emplace(turn->angleRadians, turn->engage, turn->release);
            return profile;
        }

        /** @brief Allows a teleport only when both Character query and staging ports are present. */
        bool HasTeleportDispatch(const OpenXRPlayTeleportStep *teleport) noexcept {
            return teleport && teleport->target && teleport->stage;
        }

        struct TrackedPosition final {
            XRDeviceId device;
            Math::Vec3 position;
        };

        std::optional<TrackedPosition> ReadTrackedPosition(const XRTrackingSnapshot &tracking, XRTrackedDeviceRole role,
                                                           XRTrackedPoseKind kind) {
            const auto devices = tracking.Devices();
            const auto device = std::ranges::find(devices, role, &XRTrackedDeviceRecord::role);
            if (device == devices.end() || device->state != XRTrackedDeviceState::Tracked)
                return std::nullopt;
            const auto poses = tracking.Poses();
            const auto tracked = std::ranges::find_if(poses, [&](const XRTrackedPoseRecord &pose) {
                return pose.device == device->id && pose.kind == kind;
            });
            if (tracked == poses.end() || tracked->sample.Target().kind != XRSpaceKind::World)
                return std::nullopt;
            const auto &position = tracked->sample.Components().positionMeters;
            if (position.validity != XRPoseComponentValidity::Tracked || !position.value)
                return std::nullopt;
            return TrackedPosition{device->id, *position.value};
        }

        Math::Vec3 RestorePhysicalPosition(Math::Vec3 position, const std::optional<Math::Transform> &sampledPlacement) {
            return sampledPlacement ? sampledPlacement->rotation.Inverse().Rotate(position - sampledPlacement->translation) : position;
        }

        void PopulateRoomScaleMotion(XRLocomotionSample &sample, const XRTrackingSnapshot &tracking,
                                     const std::optional<Math::Transform> &sampledPlacement,
                                     const std::optional<Math::Transform> &currentPlacement) {
            const auto head = ReadTrackedPosition(tracking, XRTrackedDeviceRole::Head, XRTrackedPoseKind::Head);
            if (!head)
                return;
            sample.head = RestorePhysicalPosition(head->position, sampledPlacement);
            sample.headSource = head->device;
            if (currentPlacement)
                sample.trackingToGameplayYawRadians = YawAroundWorldUp(currentPlacement->rotation);
        }

        void PopulatePhysicalGripMotion(XRLocomotionSample &sample, const XRTrackingSnapshot &tracking,
                                        const std::optional<Math::Transform> &sampledPlacement,
                                        const std::optional<Math::Transform> &currentPlacement) {
            const auto left = ReadTrackedPosition(tracking, XRTrackedDeviceRole::LeftController, XRTrackedPoseKind::Grip);
            const auto right = ReadTrackedPosition(tracking, XRTrackedDeviceRole::RightController, XRTrackedPoseKind::Grip);
            if (!left || !right)
                return;
            sample.leftHand = RestorePhysicalPosition(left->position, sampledPlacement);
            sample.rightHand = RestorePhysicalPosition(right->position, sampledPlacement);
            sample.handSources = std::array{left->device, right->device};
            if (currentPlacement) {
                const auto gameplay = Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, sample.gameplayYawRadians);
                sample.trackingYawRadians = YawAroundWorldUp(currentPlacement->rotation.Inverse() * gameplay);
            }
        }
    }  // namespace

    Result<void> OpenXRPlayHost::BindAuthoredCharacter(const OpenXRPlayLocomotionEvidence &evidence,
                                                       const OpenXRPlayCharacterStep &character, const Runtime::FixedStepContext &step) {
        const auto controller = character.pose.controller;
        const auto &scope = evidence.scope;
        if (authoredCharacter_ && authoredCharacter_->scene == scope.scene && authoredCharacter_->session == scope.cutoff.session &&
            authoredCharacter_->player == scope.player && authoredCharacter_->assignment == evidence.assignmentRevision &&
            authoredCharacter_->controller == controller)
            return Result<void>::Success();

        CancelAuthoredLocomotion();
        const auto &route = *AuthoredLocomotion();
        const auto *selectedTeleport = route.SelectedTeleport(evidence.comfort);
        const bool primaryTeleport = std::holds_alternative<OpenXRTeleportBinding>(route.translation);
        auto teleport = selectedTeleport ? std::make_shared<AuthoredTeleportPorts>(*this, *selectedTeleport, primaryTeleport) : nullptr;
        auto policy = XRLocomotionPolicy::Create(MakeAuthoredProfile(route, evidence.comfort), scope.cutoff.session, scope.player,
                                                 evidence.assignmentRevision, teleport);
        if (policy.HasError())
            return Result<void>::Failure(policy.ErrorValue());
        auto authority = character.issue(character.owner);
        if (authority.HasError())
            return Result<void>::Failure(authority.ErrorValue());
        const XRCharacterLocomotionBinding binding{composition_.dispatch,
                                                   scope.cutoff.session,
                                                   scope.player,
                                                   evidence.assignmentRevision,
                                                   controller,
                                                   route.speedMetersPerSecond,
                                                   teleport};
        auto adapter = XRCharacterLocomotionAdapter::CreateForFixedStep(binding, authority.Value(), step);
        if (adapter.HasError())
            return Result<void>::Failure(adapter.ErrorValue());
        authoredCharacter_.emplace(AuthoredCharacterDriver{scope.scene, scope.cutoff.session, scope.player, evidence.assignmentRevision,
                                                           controller, std::move(authority).Value(), std::move(policy).Value(),
                                                           std::move(adapter).Value(), std::move(teleport)});
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::TickAuthoredCharacter */
    Result<void> OpenXRPlayHost::TickAuthoredCharacter(Runtime::SceneRuntimeId scene, const OpenXRPlayCharacterStep &character,
                                                       const Runtime::FixedStepContext &step, const OpenXRPlayTeleportStep *teleport) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        const auto player = input_.port ? input_.port->AssignedPlayer() : std::nullopt;
        if (!AuthoredLocomotion() || !player) {
            CancelAuthoredLocomotion();
            return Result<void>::Success();
        }
        const auto tracking = CurrentInteractionTracking(*player);
        if (tracking.HasError() || scene_->scene != scene) {
            CancelAuthoredLocomotion();
            return Result<void>::Success();
        }
        const OpenXRPlayInteractionScope scope{DescribeXRInteractionCutoff(*tracking.Value()), scene, *player,
                                               *input_.interactionAssignment};
        const auto evidence = ReadLocomotionEvidence(scope, step);
        if (evidence.HasError())
            return Result<void>::Failure(evidence.ErrorValue());
        const auto &pose = character.pose;
        if (!pose.controller.IsValid() || !character.owner || !character.issue || pose.sourceTick >= character.targetTick)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        const float yaw = YawAroundWorldUp(pose.heading);

        if (auto bound = BindAuthoredCharacter(evidence.Value(), character, step); bound.HasError())
            return bound;
        const auto sample = ReadAuthoredLocomotionSample(evidence.Value(), yaw, step.fixedDelta);
        if (const auto submitted = SubmitAuthoredSample(sample, evidence.Value(), step, character.targetTick, teleport, pose.heading);
            submitted.HasError())
            return submitted;

        auto &placement = scene_->locomotion;
        if (!placement || placement->initial.controller != pose.controller)
            placement.emplace(AuthoredCharacterPlacement{pose});
        placement->pending.emplace(step.dispatchEvidence, character.targetTick);
        if (std::holds_alternative<OpenXRRoomScaleBinding>(AuthoredLocomotion()->translation)) {
            const auto worldFromPhysical = placement->worldPlacement.value_or(Math::Transform{});
            const auto pivot =
                sample.head ? worldFromPhysical.rotation.Rotate(*sample.head) + worldFromPhysical.translation : pose.position;
            placement->pending->roomScaleTurn.emplace(pivot, yaw);
        }
        return Result<void>::Success();
    }

    bool OpenXRPlayHost::IsCommittedPlacement(const PendingCharacterPlacement &pending,
                                              const Runtime::FixedStepContext &step) const noexcept {
        Runtime::RuntimeDispatchFacts facts;
        return pending.dispatch.RefersToSameDispatch(step.dispatchEvidence) &&
               step.dispatchEvidence.Read(composition_.dispatch, Runtime::RuntimePhase::FixedUpdate, facts) ==
                   Runtime::RuntimeDispatchStatus::Valid &&
               facts.committedTick == step.simulationTick && facts.committedAttempt == step.attemptNumber &&
               facts.committedFrame == step.frameNumber && facts.committedDuration == step.fixedDelta;
    }

    /** @copydoc OpenXRPlayHost::AwaitedCharacterPublication */
    std::optional<Character::CharacterControllerHandle> OpenXRPlayHost::AwaitedCharacterPublication(
        Runtime::SceneRuntimeId scene, const Runtime::FixedStepContext &step) const noexcept {
        if (std::this_thread::get_id() != lane_ || !scene_ || scene_->scene != scene || !scene_->locomotion)
            return std::nullopt;
        const auto &placement = *scene_->locomotion;
        return placement.pending && IsCommittedPlacement(*placement.pending, step) ? std::optional{placement.initial.controller}
                                                                                   : std::nullopt;
    }

    /** @copydoc OpenXRPlayHost::CommitAuthoredCharacter */
    void OpenXRPlayHost::CommitAuthoredCharacter(Runtime::SceneRuntimeId scene, const Character::CharacterTransformPublication &pose,
                                                 const Runtime::FixedStepContext &step) noexcept {
        if (AwaitedCharacterPublication(scene, step) != pose.controller)
            return;
        auto &placement = *scene_->locomotion;
        if (pose.sourceTick != placement.pending->characterTick)
            return;
        const auto roomScaleTurn = placement.pending->roomScaleTurn;
        placement.pending.reset();
        if (roomScaleTurn) {
            // The headset already physically moved. Only a committed turn changes its placement, around the tracked head.
            const auto yaw = YawAroundWorldUp(pose.heading);
            const auto turn = std::remainder(yaw - roomScaleTurn->previousYawRadians, 2.0F * std::numbers::pi_v<float>);
            if (std::abs(turn) > 0.000001F) {
                const auto rotation = Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, turn);
                const auto prior = placement.worldPlacement.value_or(Math::Transform{});
                const auto &pivot = roomScaleTurn->pivot;
                placement.worldPlacement.emplace(pivot + rotation.Rotate(prior.translation - pivot),
                                                 (rotation * prior.rotation).Normalized());
            }
            return;
        }
        const auto &initial = placement.initial;
        const auto &current = pose;
        if (current.position == initial.position && current.heading == initial.heading) {
            placement.worldPlacement.reset();
            return;
        }
        const auto rotation = (current.heading * initial.heading.Inverse()).Normalized();
        placement.worldPlacement.emplace(current.position - rotation.Rotate(initial.position), rotation);
    }

    Result<void> OpenXRPlayHost::SubmitAuthoredSample(const XRLocomotionSample &sample, const OpenXRPlayLocomotionEvidence &evidence,
                                                      const Runtime::FixedStepContext &step, std::uint64_t characterTick,
                                                      const OpenXRPlayTeleportStep *teleport, Math::Quaternion heading) {
        auto &driver = *authoredCharacter_;
        const bool hasDispatch = HasTeleportDispatch(teleport);
        if (driver.teleport && driver.teleport->primary && !hasDispatch)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto dispatch = hasDispatch ? std::optional{TeleportDispatch{*teleport, evidence, heading}} : std::nullopt;
        const auto clear = [](AuthoredTeleportPorts *ports) noexcept {
            ports->dispatch = nullptr;
        };
        std::unique_ptr<AuthoredTeleportPorts, decltype(clear)> binding{driver.teleport.get(), clear};
        if (binding && dispatch)
            binding->dispatch = &*dispatch;

        const auto frame = driver.policy.EvaluateFrame(sample, evidence.tracking);
        if (frame.HasError())
            return Result<void>::Failure(frame.ErrorValue());
        const auto admitted = SubmitValidatedLocomotion(evidence, driver.adapter, frame.Value(), step, characterTick);
        return admitted.HasError() ? Result<void>::Failure(admitted.ErrorValue()) : Result<void>::Success();
    }

    Result<std::optional<XRLocomotionTeleport>> OpenXRPlayHost::AuthoredTeleportPorts::Resolve(const XRLocomotionSample &) const {
        using Destination = Result<std::optional<XRLocomotionTeleport>>;
        if (!dispatch)
            return primary ? Destination::Failure(MakeError(XRErrors::OperationUnavailable)) : Destination::Success(std::nullopt);
        const auto devices = dispatch->evidence.tracking.Devices();
        const auto device = std::ranges::find(devices, route.role, &XRTrackedDeviceRecord::role);
        if (device == devices.end() || device->state != XRTrackedDeviceState::Tracked)
            return Destination::Success(std::nullopt);
        const std::array probes{XRInteractionProbe{device->id, XRControllerInteractionPose::Aim,
                                                   XRInteractionRayProbe{{0, 0, -1}, route.maximumDistanceMeters},
                                                   XRTrackingConfidence::High}};
        const auto candidates = play.ReadInteractionCandidates(dispatch->evidence.scope, probes);
        if (candidates.HasError())
            return Destination::Failure(candidates.ErrorValue());
        if (candidates.Value().Candidates().empty())
            return Destination::Success(std::nullopt);
        const auto &candidate = candidates.Value().Candidates().front();
        const auto &scene = dispatch->scene;
        const auto target = scene.target(scene.context, std::get<Math::Ray>(candidate.shape));
        if (target.HasError())
            return Destination::Failure(target.ErrorValue());
        if (!target.Value())
            return Destination::Success(std::nullopt);
        return Destination::Success(
            XRLocomotionTeleport{candidate.space, dispatch->evidence.tracking.Revision().Value(), *target.Value(), dispatch->heading});
    }

    Result<XRLocomotionAdmission> OpenXRPlayHost::AuthoredTeleportPorts::QueueTeleport(const XRLocomotionIntent &,
                                                                                       const Character::CharacterTeleportRequest &request,
                                                                                       const CancellationToken &lifetime) {
        if (!dispatch || lifetime.IsCancellationRequested())
            return Result<XRLocomotionAdmission>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto &scene = dispatch->scene;
        const auto staged = scene.stage(scene.context, request, lifetime);
        return staged.HasError() ? Result<XRLocomotionAdmission>::Failure(staged.ErrorValue())
                                 : Result<XRLocomotionAdmission>::Success(XRLocomotionAdmission::Queued);
    }

    XRLocomotionSample OpenXRPlayHost::ReadAuthoredLocomotionSample(const OpenXRPlayLocomotionEvidence &current, float gameplayYawRadians,
                                                                    Duration delta) const {
        const auto &authored = *AuthoredLocomotion();
        const auto readAxis = [this](const Input::ActionId &action) {
            const auto axis = input_.port->ReadCollectedAxis(action).value_or(Input::ActionValue{});
            return Math::Vec2{axis.x, axis.y};
        };
        XRLocomotionSample sample{.scope = {current.scope.cutoff, current.scope.player, current.assignmentRevision},
                                  .tick = current.fixedTick,
                                  .sequence = current.fixedTick,
                                  .fixedStepNanoseconds = delta.ToNanoseconds(),
                                  .turn = authored.turn ? readAxis(authored.turn->action).x : 0.0F,
                                  .gameplayYawRadians = gameplayYawRadians};
        const auto placement = scene_->locomotion ? scene_->locomotion->worldPlacement : std::nullopt;
        if (const auto *stick = std::get_if<OpenXRStickMoveBinding>(&authored.translation))
            sample.move = readAxis(stick->action);
        else if (std::holds_alternative<OpenXRArmSwingBinding>(authored.translation))
            PopulatePhysicalGripMotion(sample, current.tracking, input_.sampledWorldPlacement, placement);
        else if (std::holds_alternative<OpenXRRoomScaleBinding>(authored.translation))
            PopulateRoomScaleMotion(sample, current.tracking, input_.sampledWorldPlacement, placement);
        if (const auto &teleport = authoredCharacter_->teleport)
            sample.teleportPressed = input_.port->ReadCollectedButton(teleport->route.action).value_or(false);
        return sample;
    }

    /** @copydoc OpenXRPlayHost::SubmitLocomotionFrame */
    Result<XRLocomotionFrameAdmission> OpenXRPlayHost::SubmitLocomotionFrame(const OpenXRPlayInteractionScope &scope,
                                                                             XRCharacterLocomotionAdapter &adapter,
                                                                             const XRLocomotionFrame &frame,
                                                                             const Runtime::FixedStepContext &step) {
        using Outcome = Result<XRLocomotionFrameAdmission>;
        const auto evidence = ReadLocomotionEvidence(scope, step);
        if (evidence.HasError())
            return Outcome::Failure(evidence.ErrorValue());
        return SubmitValidatedLocomotion(evidence.Value(), adapter, frame, step);
    }

    /** @copydoc OpenXRPlayHost::SubmitValidatedLocomotion */
    Result<XRLocomotionFrameAdmission> OpenXRPlayHost::SubmitValidatedLocomotion(const OpenXRPlayLocomotionEvidence &current,
                                                                                 XRCharacterLocomotionAdapter &adapter,
                                                                                 const XRLocomotionFrame &frame,
                                                                                 const Runtime::FixedStepContext &step,
                                                                                 const std::optional<std::uint64_t> characterTick) {
        using Outcome = Result<XRLocomotionFrameAdmission>;
        if (frame.intent && frame.intent->scope.player != current.scope.player)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        auto admitted = adapter.SubmitFrame(frame, current.tracking, current.assignmentRevision, step, characterTick);
        if (admitted.HasValue()) {
            if (const auto &hint = admitted.Value().comfort)
                input_.comfortHint.emplace(current.scope, *hint);
            else
                input_.comfortHint.reset();
        }
        return admitted;
    }

    /** @copydoc OpenXRPlayHost::ReadFrameComfort */
    float OpenXRPlayHost::ReadFrameComfort(const Runtime::FrameContext &frame) const {
        if (!input_.comfortHint || locomotionComfort_.reducedMotion || !scene_ || !scene_->projection ||
            scene_->projection->plan.Profile() == XRFeatureProfile::FoveatedInsetProjection ||
            !Editor::EditorVulkanXrViewportAccess::SupportsComfort(composition_.viewport))
            return 0.0F;
        const auto &[scope, hint] = *input_.comfortHint;
        const auto live = CurrentInteractionTracking(scope);
        if (live.HasError())
            return 0.0F;
        const auto assignment = input_.port->ReadPlayerAssignmentRevision();
        if (!assignment)
            return 0.0F;
        const XRLocomotionScope owner{DescribeXRInteractionCutoff(*live.Value()), scope.player, *assignment};
        return std::min(hint.ReadForPresentation(owner, frame).value_or(0.0F), locomotionComfort_.maximumVignetteOpacity);
    }

    /** @copydoc OpenXRPlayHost::ReadInteractionCandidates */
    Result<XRInteractionCandidates> OpenXRPlayHost::ReadInteractionCandidates(const OpenXRPlayInteractionScope &scope,
                                                                              const std::span<const XRInteractionProbe> probes) const {
        using Outcome = Result<XRInteractionCandidates>;
        const auto current = CurrentInteractionTracking(scope);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        return BuildXRInteractionCandidates(*current.Value(), scope.cutoff, probes);
    }

    /** @copydoc OpenXRPlayHost::ReadControllerPointer */
    Result<OpenXRPlayControllerPointerSample> OpenXRPlayHost::ReadControllerPointer(
        const OpenXRPlayInteractionScope &scope, const OpenXRPlayControllerPointerRequest &request) const {
        using Outcome = Result<OpenXRPlayControllerPointerSample>;
        const auto current = CurrentInteractionTracking(scope);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        const auto &bindings = input_.coordinator->Current()->bindings;
        const auto binding = std::ranges::find(bindings, request.selectAction, &XRResolvedActionBinding::action);
        if (binding == bindings.end() || binding->valueType != Input::ActionValueType::Digital)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto index = static_cast<std::uint32_t>(binding - bindings.begin());
        const auto actions = current.Value()->Actions();
        const auto select = std::ranges::find_if(actions, [&](const XRTrackedActionRecord &action) {
            return action.bindingIndex == index && action.device == request.device;
        });
        if (select == actions.end() || !select->active)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        const XRInteractionProbe probe{request.device, request.pose, request.ray, request.minimumConfidence};
        const auto geometry = BuildXRInteractionCandidates(*current.Value(), scope.cutoff, std::span{&probe, 1});
        if (geometry.HasError())
            return Outcome::Failure(geometry.ErrorValue());
        const auto &candidate = geometry.Value().Candidates().front();
        return Outcome::Success({std::get<Math::Ray>(candidate.shape), select->x != 0.0F, candidate.space, candidate.confidence});
    }

    /** @copydoc OpenXRPlayHost::ReadWorldUiPointer */
    std::optional<OpenXRWorldUiPointerFrame> OpenXRPlayHost::ReadWorldUiPointer(const Input::PlayerId player) const {
        if (std::this_thread::get_id() != lane_ || busy_ || closing_)
            return std::nullopt;
        const auto &selected = composition_.bindings->worldUiPointer;
        if (!selected)
            return std::nullopt;
        const auto current = ReadInteractionSources(player);
        if (current.HasError())
            return std::nullopt;
        const auto &sources = current.Value();
        const auto devices = sources.Devices();
        const auto device = std::ranges::find(devices, selected->role, &XRTrackedDeviceRecord::role);
        if (device == devices.end())
            return std::nullopt;
        const auto &scope = sources.Scope();
        // OpenXR Aim points along its negative local Z axis.
        const auto sample = ReadControllerPointer(scope, {device->id,
                                                          selected->action,
                                                          XRControllerInteractionPose::Aim,
                                                          {{0, 0, -1}, selected->maximumDistanceMeters},
                                                          XRTrackingConfidence::High});
        if (sample.HasError())
            return std::nullopt;
        return OpenXRWorldUiPointerFrame{{device->id, scope.cutoff.origin, scope.cutoff.bindingRevision, scope.scene, scope.player,
                                          scope.assignmentRevision},
                                         sample.Value(),
                                         scope.cutoff,
                                         sources.captureTime_};
    }

    /** @copydoc OpenXRPlayHost::BuildInputSnapshot */
    Result<void> OpenXRPlayHost::BuildInputSnapshot(const Runtime::FrameContext &context, const Input::RawInputSnapshot &raw) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!input_.router)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const Operation operation{*this};
        const bool neutral = std::exchange(input_.neutralPending, false);
        if (input_.admitted && scene_ && scene_->geometry && scene_->geometry->mounted) {
            if (const auto current = scene_->geometry->ValidateSource(); current.HasError()) {
                scene_->Retire(*this);
                input_.router->BeginFrame(raw);
                return current;
            }
        }
        if (input_.port && !eventsPending_) {
            const auto refreshed = input_.router->RefreshPlayerActionCollection(*input_.port);
            if (refreshed.HasError()) {
                RevokeSource();
                input_.router->BeginFrame(raw);
                return Result<void>::Failure(refreshed.ErrorValue());
            }
            if (refreshed.Value()) {
                tracking_.Revoke();
                if (input_.admitted)
                    composition_.session.RetireInputProjection();
                input_.router->BeginFrame(raw);
                if (input_.admitted && input_.coordinator) {
                    const auto rebound = composition_.session.RebuildInputProjection(*input_.port, *input_.coordinator,
                                                                                     *composition_.bindings, input_.router->Actions());
                    if (rebound.HasError())
                        LOG_WARN("editor.xr", "Current action map cannot admit the signed XR input plan: %s",
                                 rebound.ErrorValue().code.Value().c_str());
                }
                return Result<void>::Success();
            }
        }
        if (neutral || !input_.port || !input_.admitted || eventsPending_ || input_.play != Editor::EditorPlayState::Playing ||
            !composition_.session.HasInputProjection()) {
            tracking_.Revoke();
            input_.router->BeginFrame(raw);
            return Result<void>::Success();
        }
        auto opened = input_.port->Begin(raw);
        if (opened.HasError()) {
            RevokeSource();
            input_.router->BeginFrame(raw);
            return Result<void>::Failure(opened.ErrorValue());
        }
        auto frame = std::move(opened).Value();
        // The first raw commit can enable routing; assignment evidence must precede native callbacks.
        const auto interactionAssignment = input_.router->RoutingState({}).assignmentRevision;
        const auto sampled = AppendInput(context, frame);
        const bool profileChanged = sampled.HasError() && RetireIfInputProfileChanged(sampled.ErrorValue());
        if (pendingRevoke_) {
            if (const auto canceled = frame.Cancel(); canceled.HasError())
                return canceled;
            // The native call has returned. Retire registration before committing its neutral cutoff.
            pendingRevoke_ = false;
            composition_.session.RevokeInput();
            input_.router->BeginFrame(raw);
            if (profileChanged)
                return Result<void>::Success();
            return sampled.HasError() ? sampled : Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        }
        // One empty-source commit on native failure preserves physical edges and neutralizes old XR values.
        if (const auto committed = frame.Commit(); committed.HasError()) {
            RevokeSource();
            return committed;
        }
        if (sampled.HasError()) {
            RevokeSource();
            return sampled;
        }
        input_.interactionAssignment = interactionAssignment;
        if (const auto *tracking = tracking_.Current()) {
            if (const auto advisory = composition_.session.ReadStageBoundary(*tracking))
                composition_.metrics.RecordStageBoundary(*advisory);
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::ReleaseInputOwner */
    void OpenXRPlayHost::ReleaseInputOwner() noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_ && !busy_, "XR Input owner release requires its idle owner lane");
        RevokeInput();
        input_.coordinator.reset();
        input_.play = Editor::EditorPlayState::Idle;
        input_.router = nullptr;
    }
}  // namespace Horo::Application::Internal
