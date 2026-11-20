#include "OpenXRPlayHost.h"

#include "Horo/Foundation/Assertions.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "OpenXRPlayFrameState.h"

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
                       composition.lighting},
          native_(preparation.native), discoveryAttempt_(preparation.preflight.Attempt()), getProc_(preparation.getProc), clock_(native_) {
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
            composition_.session.RequestHapticCancellation();
            RevokeEyeGaze();
            RevokePlaneCapture();
            RevokePassthrough();
        }
        if (state == Idle || state == Stopping || state == Failed)
            RevokeSource();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::RevokeSource */
    void OpenXRPlayHost::RevokeSource() noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_, "XR Play revocation requires its owner lane");
        input_.admitted = false;
        tracking_.Revoke();
        RevokeEyeGaze();
        RevokePlaneCapture();
        RevokePassthrough();
        composition_.session.RequestHapticCancellation();
        if (busy_) {
            if (scene_ && !scene_->activated)
                scene_->preparationCancellation.RequestCancellation();
            pendingRevoke_ = true;
            return;
        }
        composition_.session.RevokeInput();
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
        if (sessions_.Snapshot().state == XRSessionState::Lost && scene_)
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
        if (auto tail = ProgressTail(); tail.HasError()) {
            RevokeEventAdmission();
            return tail;
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::ApplyEventObservation */
    Result<bool> OpenXRPlayHost::ApplyEventObservation(const NativeSessionEventObservation &observed) {
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
        const auto state = sessions_.Snapshot();
        if (observed.actionsInterrupted) {
            tracking_.Revoke();
            RevokeEyeGaze();
            RevokePlaneCapture();
            RevokePassthrough();
            input_.neutralPending = true;
        }
        const bool invalidateOutput = observed.actionsInterrupted || observed.bindingsChanged;
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
        if (const auto state = sessions_.Snapshot(); state.session.IsValid()) {
            // The copied publication is current on this serialized lane. SessionLost is legal from
            // every published state and only revokes admission; it never destroys native children.
            const auto lost = sessions_.ApplyEvent(state.session, XRSessionEvent::SessionLost);
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
        return composition_.session.AppendInput(sessions_, *input_.coordinator, tracking_, clock_, context, input_.origin, frame);
    }

    /** @copydoc OpenXRPlayHost::ReadReferences */
    Result<XRReferenceSpaceSnapshot> OpenXRPlayHost::ReadReferences(XRSessionId session) const {
        using Outcome = Result<XRReferenceSpaceSnapshot>;
        if (auto lane = Lane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        const auto state = sessions_.Snapshot();
        if (auto valid = ValidateXRSession(session, state.session); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (state.state == XRSessionState::Lost || !scene_ || !scene_->published || scene_->retiring)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (eventsPending_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        return Outcome::Success(scene_->references);
    }

    /** @copydoc OpenXRPlayHost::ReadReferenceChange */
    std::optional<XRReferenceSpaceChange> OpenXRPlayHost::ReadReferenceChange(XRSessionId session) const noexcept {
        if (std::this_thread::get_id() != lane_ || busy_ || closing_ || !lastReferenceChange_ || lastReferenceChange_->session != session)
            return std::nullopt;
        const auto active = sessions_.Snapshot().session;
        return active.IsValid() && active != session ? std::nullopt : lastReferenceChange_;
    }

    /** @copydoc OpenXRPlayHost::ReadHandGestures */
    XRHandGestureCandidates OpenXRPlayHost::ReadHandGestures() const noexcept {
        const auto *snapshot = tracking_.Current();
        if (!snapshot || eventsPending_ || !input_.admitted || input_.play != Editor::EditorPlayState::Playing)
            return {};
        return composition_.session.ReadHandGestures(*snapshot);
    }

    /** @copydoc OpenXRPlayHost::HasCurrentAssignedInput */
    bool OpenXRPlayHost::HasCurrentAssignedInput() const {
        if (!input_.router || !input_.port || !input_.port->AssignedPlayer())
            return false;
        auto port = *input_.port;
        const auto refresh = input_.router->RefreshPlayerActionCollection(port);
        return refresh.HasValue() && !refresh.Value();
    }

    /** @copydoc OpenXRPlayHost::CurrentInteractionTracking */
    Result<const XRTrackingSnapshot *> OpenXRPlayHost::CurrentInteractionTracking(const Input::PlayerId player) const {
        using Outcome = Result<const XRTrackingSnapshot *>;
        if (const auto lane = Lane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        if (!input_.port || input_.port->AssignedPlayer() != player)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const auto *snapshot = tracking_.Current();
        const auto state = sessions_.Snapshot();
        if (!snapshot || !scene_ || !scene_->published || scene_->retiring || !state.admitsActions || eventsPending_ || !input_.admitted ||
            input_.play != Editor::EditorPlayState::Playing || snapshot->Focus() != XRTrackingFocusState::Focused ||
            snapshot->Visibility() != XRTrackingVisibilityState::Visible || snapshot->State() != XRTrackingState::Active)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (scene_->session != state.session || snapshot->Session() != state.session || snapshot->WorldOriginRevision() != input_.origin ||
            !input_.coordinator || !input_.coordinator->Current() || input_.coordinator->Revision() != snapshot->BindingRevision() ||
            !HasCurrentAssignedInput())
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
        if (scope.cutoff != DescribeXRInteractionCutoff(*current.Value()))
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        return current;
    }

    /** @copydoc OpenXRPlayHost::ReadInteractionSources */
    Result<OpenXRPlayInteractionSources> OpenXRPlayHost::ReadInteractionSources(const Input::PlayerId player) const {
        using Outcome = Result<OpenXRPlayInteractionSources>;
        const auto current = CurrentInteractionTracking(player);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        const auto &snapshot = *current.Value();
        OpenXRPlayInteractionSources sources;
        sources.scope_ = {DescribeXRInteractionCutoff(snapshot), scene_->scene, player};
        using enum XRTrackedDeviceCapability;
        for (const auto &device : snapshot.Devices()) {
            const auto &capabilities = device.capabilities;
            if (device.state == XRTrackedDeviceState::Tracked &&
                (capabilities.Has(AimPose) || capabilities.Has(GripPose) || capabilities.Has(HandJoints)))
                sources.devices_[sources.count_++] = device;
        }
        return Outcome::Success(std::move(sources));
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
        return Outcome::Success({std::get<Math::Ray>(geometry.Value().Candidates().front().shape), select->x != 0.0F});
    }

    /** @copydoc OpenXRPlayHost::ReadWorldUiPointer */
    std::optional<OpenXRWorldUiPointerFrame> OpenXRPlayHost::ReadWorldUiPointer(const Input::PlayerId player) const {
        const auto &selected = composition_.bindings->worldUiPointer;
        if (!selected || !input_.port || !input_.port->RoutingEligible())
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
        return OpenXRWorldUiPointerFrame{{device->id, scope.cutoff.origin, scope.cutoff.bindingRevision, scope.scene, scope.player},
                                         sample.Value()};
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
