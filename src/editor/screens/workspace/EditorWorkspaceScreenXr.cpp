#include "EditorWorkspaceScreen.h"
#include "Horo/Foundation/Logging/Logger.h"

namespace Horo::Editor::WorkspaceScreenDetail {
    /** @copydoc EditorWorkspaceScreen::~EditorWorkspaceScreen */
    EditorWorkspaceScreen::~EditorWorkspaceScreen() {
        if (xr_.attached)
            xr_.host->RevokeInput();
    }

    /** @copydoc EditorWorkspaceScreen::AttachXrInput */
    Result<void> EditorWorkspaceScreen::AttachXrInput() {
        if (!xr_.host)
            return Result<void>::Success();  // Ordinary desktop has no explicitly selected XR producer.
        auto port = input_.router.ActionCollection(input_.context);
        if (port.HasError())
            return Result<void>::Failure(port.ErrorValue());
        // Set the cleanup obligation before calling a host which may throw after partial preparation.
        xr_.attached = true;
        if (const auto attached = xr_.host->AttachInput(port.Value(), controller_->XrSceneSource()); attached.HasError()) {
            xr_.host->RevokeInput();
            xr_.attached = false;
            return attached;
        }
        controller_->SelectXrPlayHost(xr_.host);
        xr_.observed.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceScreen::ObserveXrPlayState */
    void EditorWorkspaceScreen::ObserveXrPlayState() {
        if (!xr_.attached || !controller_)
            return;
        const auto state = controller_->ViewModel().playState;
        if (xr_.observed == state)
            return;
        if (const auto observed = xr_.host->SetPlayState(state); observed.HasError()) {
            // The host closes its own producer first; this forwards the failure to the existing Play owner.
            LOG_ERROR("editor.xr", "XR Play admission failed: %s", observed.ErrorValue().message.c_str());
            controller_->ProcessCommand({.command = EditorWorkspaceViewCommand::StopPlay});
            xr_.observed = controller_->ViewModel().playState;
            return;
        }
        xr_.observed = state;
    }

    /** @copydoc EditorWorkspaceScreen::DetachXrInput */
    void EditorWorkspaceScreen::DetachXrInput() {
        if (!xr_.attached)
            return;
        controller_->SelectXrPlayHost(nullptr);
        const auto detached = xr_.host->DetachInput();
        xr_.host->RevokeInput();  // Non-allocating fail-closed cleanup before the workspace context is released.
        xr_.attached = false;
        xr_.observed.reset();
        if (detached.HasError())
            LOG_ERROR("editor.xr", "XR Input retirement remains owned by the application: %s", detached.ErrorValue().message.c_str());
    }
}  // namespace Horo::Editor::WorkspaceScreenDetail
