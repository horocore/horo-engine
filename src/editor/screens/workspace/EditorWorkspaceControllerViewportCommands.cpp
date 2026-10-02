#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {

    /** @copydoc EditorWorkspaceController::ApplyViewportCameraChange */
    void EditorWorkspaceController::ApplyViewportCameraChange(const Result<void> &result, const char *action) {
        if (result.HasError()) {
            LOG_ERROR("editor.viewport", "%s failed: %s", action, result.ErrorValue().message.c_str());
            return;
        }
        m_viewportScene.camera = m_viewport.Current().camera;
        m_viewModel.viewportCamera = m_viewport.Current().camera;
    }

    bool EditorWorkspaceController::ProcessViewportCameraCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::NavigateViewport: {
                if (!cmd.viewportNavigationPayload.has_value())
                    break;
                ApplyViewportCameraChange(m_viewport.Navigate(*cmd.viewportNavigationPayload), "Viewport navigation");
                break;
            }
            case EditorWorkspaceViewCommand::AlignViewportToAxis: {
                if (!cmd.viewportAxisPayload.has_value())
                    break;
                ApplyViewportCameraChange(m_viewport.AlignToAxis(*cmd.viewportAxisPayload), "Viewport axis alignment");
                break;
            }
            case EditorWorkspaceViewCommand::ChangeViewportProjection: {
                if (!cmd.viewportProjectionPayload.has_value())
                    break;
                ApplyViewportCameraChange(m_viewport.SetProjection(*cmd.viewportProjectionPayload), "Viewport projection change");
                break;
            }
            case EditorWorkspaceViewCommand::FocusViewportSelection: {
                if (!m_viewModel.primarySelectionWorldBounds.has_value() || !cmd.floatPayload.has_value())
                    break;
                ApplyViewportCameraChange(m_viewport.Focus(*m_viewModel.primarySelectionWorldBounds, *cmd.floatPayload), "Viewport focus");
                break;
            }
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
