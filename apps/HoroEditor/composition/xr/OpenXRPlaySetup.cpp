#include "Horo/XR/XRSessionErrors.h"
#include "OpenXRPlayHost.h"

#include <algorithm>
#include <limits>

namespace Horo::Application::Internal {
    namespace {}  // namespace

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
        using enum XR::XRCapability;
        constexpr std::array kinds{Projection, ViewSpace, LocalSpace, StageSpace, CanonicalInputProjection, HandTracking};
        static_assert(kinds.size() == Editor::EditorXrInspectionSnapshot::CapabilityCount);
        for (std::size_t index = 0; index < kinds.size(); ++index) {
            const auto kind = kinds[index];
            report.capabilities[index] = {kind, scene_->projection->capabilities.State(kind), plan->Decision(kind).state};
        }

        if (eventsPending_)
            report.status = EventsPending;
        else if (!state.admitsActions)
            report.status = Unfocused;
        else if (const auto *tracking = tracking_.Current(); tracking && tracking->Session() == state.session &&
                                                             tracking->WorldOriginRevision() == input_.origin && input_.admitted &&
                                                             input_.play == Editor::EditorPlayState::Playing) {
            CopyOpenXRInspectionTracking(report, *tracking);
            report.status = Current;
        } else
            report.status = WaitingForSample;
        return report;
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
        const auto result = composition_.session.RunSetupSessionTest(*composition_.bindings);
        if (result.HasError()) {
            setup_.issue = SetupIssue(result.ErrorValue());
            setupCleanupPending_ = setup_.issue == Editor::EditorXrSetupIssue::Cleanup;
            return;
        }
        setup_.test = Editor::EditorXrSetupTestState::Passed;
        setup_.issue = Editor::EditorXrSetupIssue::None;
    }
}  // namespace Horo::Application::Internal
