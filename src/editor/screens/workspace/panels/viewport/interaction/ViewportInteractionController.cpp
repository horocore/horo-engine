#include "ViewportInteractionController.h"

namespace Horo::Editor {
    ViewportInteractionController::ViewportInteractionController() noexcept : capture_(*this) {}

    void ViewportInteractionController::Attach(Input::InputRouter &router, Input::InputContextToken &workspaceContext) noexcept {
        capture_.Attach(router, workspaceContext);
    }

    void ViewportInteractionController::Detach() noexcept {
        capture_.Detach();
        navigation_.Reset();
        gizmo_.Reset();
        colliders_.Reset();
    }

    bool ViewportInteractionController::IsActive() const noexcept {
        return navigation_.IsActive() || gizmo_.IsActive() || colliders_.IsActive();
    }

    bool ViewportInteractionController::ConsumePendingCancellation(EditorWorkspaceViewCommandData &command) noexcept {
        return colliders_.ConsumePendingCancellation() || gizmo_.ConsumePendingCancellation(command);
    }

    /** @copydoc ViewportInteractionController::CancelForUnavailableSurface */
    void ViewportInteractionController::CancelForUnavailableSurface(EditorWorkspaceViewCommandData &command) noexcept {
        capture_.Cancel(Input::CaptureCancellationReason::Explicit);
        OnViewportCaptureCancelled(Input::CaptureCancellationReason::Explicit);
        static_cast<void>(ConsumePendingCancellation(command));
    }

    void ViewportInteractionController::Draw(const ViewportInteractionDrawContext &context) {
        const Input::InputRouter *router = capture_.Router();
        if (router == nullptr || capture_.WorkspaceContext() == nullptr)
            return;
        const Input::RawInputSnapshot &input = router->Snapshot();
        if (const TransformGizmoDrawContext gizmoContext{
                .origin = context.origin,
                .width = context.width,
                .height = context.height,
                .hovered = context.hovered,
                .input = input,
                .viewModel = context.viewModel,
                .command = context.command,
                .depthRange = context.depthRange,
            };
            colliders_.Draw(context.drawList, gizmoContext, capture_, context.gui) ||
            (context.viewModel.selectedColliders.empty() && gizmo_.Draw(context.drawList, gizmoContext, capture_)))
            return;
        navigation_.Update(
            ViewportNavigationUpdateContext{
                .origin = context.origin,
                .width = context.width,
                .height = context.height,
                .hovered = context.hovered,
                .input = input,
                .viewModel = context.viewModel,
                .command = context.command,
                .gui = context.gui,
                .deltaSeconds = context.deltaSeconds,
                .depthRange = context.depthRange,
            },
            capture_);
    }

    void ViewportInteractionController::OnViewportCaptureCancelled(const Input::CaptureCancellationReason) noexcept {
        navigation_.Reset();
        gizmo_.OnCaptureCancelled();
        colliders_.OnCaptureCancelled();
    }
}  // namespace Horo::Editor
