#include "Horo/Editor/EditorWorkspaceEvents.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/document/EditorViewportPicking.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Editor {
    bool EditorWorkspaceController::ProcessViewportPickCommand(const EditorWorkspaceViewCommandData &cmd) {
        if (cmd.command != EditorWorkspaceViewCommand::PickViewport)
            return false;
        if (cmd.viewportPickPayload.has_value())
            HandleViewportPick(*cmd.viewportPickPayload);
        return true;
    }

    void EditorWorkspaceController::HandleViewportPick(const ViewportPickRequest &request) {
        const Result<EditorViewportPickResult> picked = PickEditorViewportScene(m_viewportScene, EditorViewportPickQuery{
                                                                                                     .normalizedX = request.normalizedX,
                                                                                                     .normalizedY = request.normalizedY,
                                                                                                     .aspect = request.aspect,
                                                                                                     .depthRange = request.depthRange,
                                                                                                 });
        if (picked.HasError()) {
            LOG_ERROR("editor.viewport_picking", "Viewport pick failed: %s", picked.ErrorValue().message.c_str());
            return;
        }
        if (const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
            !active || picked.Value().runtimeScene != active->RuntimeId()) {
            LOG_WARN("editor.viewport_picking", "Discarded a stale runtime-scene pick result.");
            return;
        }
        ApplyViewportPickSelection(picked.Value(), request);
        RefreshSelectionProjection();
    }

    void EditorWorkspaceController::ApplyViewportPickSelection(const EditorViewportPickResult &picked, const ViewportPickRequest &request) {
        if (!picked.object.has_value()) {
            if (!request.toggleSelection)
                m_selection.Clear();
            return;
        }

        const SceneObjectId object = *picked.object;
        std::vector<SceneObjectId> objects;
        std::optional<SceneObjectId> primary = object;
        if (request.toggleSelection) {
            objects = m_selection.Current().objects;
            if (const auto existing = std::ranges::find(objects, object); existing == objects.end()) {
                objects.push_back(object);
            } else {
                objects.erase(existing);
                primary = objects.empty() ? std::nullopt : std::optional{objects.back()};
            }
        } else {
            objects.push_back(object);
        }
        if (const Result<void> selected = m_selection.SetObjects(objects, primary); selected.HasError())
            LOG_ERROR("editor.selection", "Viewport selection failed: %s", selected.ErrorValue().message.c_str());
    }

    bool EditorWorkspaceController::ProcessViewportCommand(const EditorWorkspaceViewCommandData &cmd) {
        return ProcessViewportCameraCommand(cmd) || ProcessViewportEditCommand(cmd);
    }

    bool EditorWorkspaceController::ProcessViewportEditCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::PreviewObjectTransform:
                if (cmd.transformUpdates.has_value()) {
                    PreviewObjectTransforms(*cmd.transformUpdates);
                } else if (cmd.objectPayload.has_value() && cmd.transformPayload.has_value()) {
                    PreviewObjectTransform(*cmd.objectPayload, *cmd.transformPayload);
                }
                break;
            case EditorWorkspaceViewCommand::CommitObjectTransform:
                if (cmd.transformUpdates.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(SetSceneObjectTransformsCommand{*cmd.transformUpdates}),
                                                "Transform objects");
                } else if (cmd.objectPayload.has_value() && cmd.transformPayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    SetSceneObjectTransformCommand{*cmd.objectPayload, *cmd.transformPayload}),
                                                "Transform object");
                }
                break;
            case EditorWorkspaceViewCommand::CancelObjectTransformPreview:
                CancelObjectTransformPreview();
                break;
            case EditorWorkspaceViewCommand::PreviewLightComponent:
                if (cmd.objectPayload.has_value() && cmd.lightPayload.has_value())
                    PreviewLightComponent(*cmd.objectPayload, *cmd.lightPayload);
                break;
            case EditorWorkspaceViewCommand::CancelLightComponentPreview:
                CancelLightComponentPreview();
                break;
            default:
                return false;
        }
        return true;
    }

    bool EditorWorkspaceController::ProcessComponentCommand(const EditorWorkspaceViewCommandData &cmd) {
        return ProcessObjectPropertyCommand(cmd) || ProcessBehaviorCommand(cmd);
    }

    bool EditorWorkspaceController::ProcessContentBrowserCommand(const EditorWorkspaceViewCommandData &cmd) {
        return ProcessContentBrowserNavigationCommand(cmd) || ProcessContentBrowserMutationCommand(cmd);
    }

    bool EditorWorkspaceController::ProcessContentBrowserNavigationCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::NavigateContentBrowser:
                if (cmd.stringPayload.has_value())
                    NavigateContentBrowser(*cmd.stringPayload, true);
                break;
            case EditorWorkspaceViewCommand::NavigateContentBrowserBack:
                NavigateContentBrowserBack();
                break;
            case EditorWorkspaceViewCommand::NavigateContentBrowserForward:
                NavigateContentBrowserForward();
                break;
            case EditorWorkspaceViewCommand::NavigateContentBrowserUp:
                NavigateContentBrowserUp();
                break;
            case EditorWorkspaceViewCommand::RefreshContentBrowser:
                RequestContentBrowserRefresh();
                break;
            default:
                return false;
        }
        return true;
    }

    bool EditorWorkspaceController::ProcessContentBrowserMutationCommand(const EditorWorkspaceViewCommandData &cmd) {
        return ProcessContentBrowserAssetCommand(cmd) || ProcessContentBrowserSourceCommand(cmd);
    }

}  // namespace Horo::Editor
