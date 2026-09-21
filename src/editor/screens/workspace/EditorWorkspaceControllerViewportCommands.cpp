#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {

    bool EditorWorkspaceController::ProcessViewportCameraCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::NavigateViewport:
                if (cmd.viewportNavigationPayload.has_value()) {
                    const Result<void> navigated = m_viewport.Navigate(*cmd.viewportNavigationPayload);
                    if (navigated.HasError()) {
                        LOG_ERROR("editor.viewport", "Viewport navigation failed: %s", navigated.ErrorValue().message.c_str());
                    } else {
                        m_viewportScene.camera = m_viewport.Current().camera;
                        m_viewModel.viewportCamera = m_viewport.Current().camera;
                    }
                }
                break;
            case EditorWorkspaceViewCommand::ChangeViewportProjection:
                if (cmd.viewportProjectionPayload.has_value()) {
                    const Result<void> changed = m_viewport.SetProjection(*cmd.viewportProjectionPayload);
                    if (changed.HasError())
                        LOG_ERROR("editor.viewport", "Viewport projection change failed: %s", changed.ErrorValue().message.c_str());
                    else {
                        m_viewportScene.camera = m_viewport.Current().camera;
                        m_viewModel.viewportCamera = m_viewport.Current().camera;
                    }
                }
                break;
            case EditorWorkspaceViewCommand::FocusViewportSelection:
                if (m_viewModel.primarySelectionWorldBounds.has_value() && cmd.floatPayload.has_value()) {
                    const Result<void> focused = m_viewport.Focus(*m_viewModel.primarySelectionWorldBounds, *cmd.floatPayload);
                    if (focused.HasError())
                        LOG_ERROR("editor.viewport", "Viewport focus failed: %s", focused.ErrorValue().message.c_str());
                    else {
                        m_viewportScene.camera = m_viewport.Current().camera;
                        m_viewModel.viewportCamera = m_viewport.Current().camera;
                    }
                }
                break;
            case EditorWorkspaceViewCommand::ChangeTransformTool:
                if (cmd.transformToolPayload.has_value())
                    m_viewModel.activeTransformTool = *cmd.transformToolPayload;
                break;
            case EditorWorkspaceViewCommand::ChangeTransformSpace:
                if (cmd.transformSpacePayload.has_value())
                    m_viewModel.activeTransformSpace = *cmd.transformSpacePayload;
                break;
            default:
                return false;
        }
        return true;
    }

}  // namespace Horo::Editor
