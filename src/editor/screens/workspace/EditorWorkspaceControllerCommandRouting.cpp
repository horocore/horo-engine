#include "Horo/Editor/EditorWorkspaceEvents.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <filesystem>

namespace Horo::Editor {
    void EditorWorkspaceController::ProcessCommand(const EditorWorkspaceViewCommandData &cmd) {
        // A transient overlay must not survive the interaction or workspace action that owns it.
        if (!m_viewport.Current().transformPreviews.empty() && cmd.command != EditorWorkspaceViewCommand::None &&
            cmd.command != EditorWorkspaceViewCommand::PreviewObjectTransform &&
            cmd.command != EditorWorkspaceViewCommand::CommitObjectTransform &&
            cmd.command != EditorWorkspaceViewCommand::CancelObjectTransformPreview) {
            CancelObjectTransformPreview();
        }
        if (m_viewport.Current().lightPreview.has_value() && cmd.command != EditorWorkspaceViewCommand::None &&
            cmd.command != EditorWorkspaceViewCommand::PreviewLightComponent &&
            cmd.command != EditorWorkspaceViewCommand::UpdateLightComponent &&
            cmd.command != EditorWorkspaceViewCommand::CancelLightComponentPreview) {
            CancelLightComponentPreview();
        }
        if (m_assetPlacementPreviewActive && cmd.command != EditorWorkspaceViewCommand::None &&
            cmd.command != EditorWorkspaceViewCommand::PreviewAssetPlacement &&
            cmd.command != EditorWorkspaceViewCommand::InstantiateAsset &&
            cmd.command != EditorWorkspaceViewCommand::CancelAssetPlacementPreview) {
            CancelAssetPlacementPreview();
        }
        static_cast<void>(ProcessDocumentCommand(cmd) || ProcessPlayCommand(cmd) || ProcessSceneObjectCommand(cmd) ||
                          ProcessViewportPickCommand(cmd) || ProcessViewportCommand(cmd) || ProcessComponentCommand(cmd) ||
                          ProcessContentBrowserCommand(cmd) || ProcessActivePanelCommand(cmd) || ProcessLayoutCommand(cmd));
    }

    bool EditorWorkspaceController::ProcessDocumentCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::None:
            case EditorWorkspaceViewCommand::ReturnToWelcome:
            case EditorWorkspaceViewCommand::CompareExternalScene:
                break;
            case EditorWorkspaceViewCommand::SaveScene:
                SaveScene();
                break;
            case EditorWorkspaceViewCommand::SaveSceneAs:
                if (cmd.stringPayload.has_value())
                    SaveSceneToPath(std::filesystem::path{*cmd.stringPayload}, false);
                break;
            case EditorWorkspaceViewCommand::SaveSceneCopyAs:
                if (cmd.stringPayload.has_value())
                    SaveSceneToPath(std::filesystem::path{*cmd.stringPayload}, true);
                break;
            case EditorWorkspaceViewCommand::ReloadExternalScene:
                ReloadExternalScene();
                break;
            case EditorWorkspaceViewCommand::OverwriteExternalScene:
                SaveScene(true);
                break;
            case EditorWorkspaceViewCommand::RestoreSceneRecovery:
                RestoreSceneRecovery();
                break;
            case EditorWorkspaceViewCommand::DiscardSceneRecovery:
                DiscardSceneRecovery();
                break;
            case EditorWorkspaceViewCommand::UndoScene:
                HandleDocumentCommandResult(m_documentCommands.Undo(), "Undo");
                break;
            case EditorWorkspaceViewCommand::RedoScene:
                HandleDocumentCommandResult(m_documentCommands.Redo(), "Redo");
                break;
            default:
                return false;
        }
        return true;
    }

    bool EditorWorkspaceController::ProcessPlayCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::StartPlay:
                StartPlaySession();
                break;
            case EditorWorkspaceViewCommand::PausePlay: {
                if (const Result<void> paused = m_playSession.Pause(); paused.HasError())
                    LOG_WARN("editor.play_mode", "%s", paused.ErrorValue().message.c_str());
                RefreshPlayStateProjection();
                break;
            }
            case EditorWorkspaceViewCommand::ResumePlay: {
                if (const Result<void> resumed = m_playSession.Resume(); resumed.HasError())
                    LOG_WARN("editor.play_mode", "%s", resumed.ErrorValue().message.c_str());
                RefreshPlayStateProjection();
                break;
            }
            case EditorWorkspaceViewCommand::StepPlay: {
                if (const Result<void> stepped = m_playSession.Step(); stepped.HasError())
                    LOG_WARN("editor.play_mode", "%s", stepped.ErrorValue().message.c_str());
                RefreshPlayStateProjection();
                break;
            }
            case EditorWorkspaceViewCommand::StopPlay:
                StopPlaySession();
                break;
            default:
                return false;
        }
        return true;
    }

    bool EditorWorkspaceController::ProcessSceneObjectCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::CreatePrimitive:
                if (cmd.primitivePayload.has_value())
                    HandleCreatePrimitive(*cmd.primitivePayload, cmd.objectPayload);
                break;
            case EditorWorkspaceViewCommand::PreviewAssetPlacement:
                if (cmd.assetSceneDrop.has_value())
                    PreviewAssetPlacement(*cmd.assetSceneDrop);
                break;
            case EditorWorkspaceViewCommand::CancelAssetPlacementPreview:
                CancelAssetPlacementPreview();
                break;
            case EditorWorkspaceViewCommand::InstantiateAsset:
                if (cmd.assetSceneDrop.has_value())
                    HandleInstantiateAsset(*cmd.assetSceneDrop);
                break;
            case EditorWorkspaceViewCommand::DuplicateObject:
                if (cmd.objectPayload.has_value())
                    HandleDuplicateObject(*cmd.objectPayload);
                break;
            case EditorWorkspaceViewCommand::DeleteObject:
                if (cmd.objectPayload.has_value())
                    HandleDeleteObject(*cmd.objectPayload);
                break;
            case EditorWorkspaceViewCommand::DeleteSelectedObjects:
                if (cmd.objectSelection.has_value())
                    HandleDeleteSelectedObjects(cmd.objectSelection->objects);
                break;
            case EditorWorkspaceViewCommand::SelectObject:
                if (cmd.objectSelection.has_value() || cmd.objectPayload.has_value()) {
                    ObjectSelectionRequest request;
                    if (cmd.objectSelection.has_value()) {
                        request = *cmd.objectSelection;
                    } else {
                        request.objects = {*cmd.objectPayload};
                        request.primary = *cmd.objectPayload;
                    }
                    if (const Result<void> selected = m_selection.SetObjects(request.objects, request.primary); selected.HasError())
                        LOG_ERROR("editor.selection", "Select object failed: %s", selected.ErrorValue().message.c_str());

                    RefreshSelectionProjection();
                }
                break;
            default:
                return false;
        }
        return true;
    }
}  // namespace Horo::Editor
