#include "Horo/XR/XRSessionErrors.h"
#include "OpenXRPlayHost.h"

#include <limits>

namespace Horo::Application::Internal {
    namespace {
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
    }  // namespace

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
