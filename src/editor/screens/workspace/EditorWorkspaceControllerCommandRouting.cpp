#include "Horo/Editor/EditorWorkspaceEvents.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <filesystem>

namespace Horo::Editor {
    void EditorWorkspaceController::ProcessCommand(const EditorWorkspaceViewCommandData &cmd) {
        if (m_prefabRefreshing)
            return;
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
        if ((m_assetPlacementPreviewActive || m_prefabDrop) && cmd.command != EditorWorkspaceViewCommand::None &&
            cmd.command != EditorWorkspaceViewCommand::PreviewAssetPlacement &&
            cmd.command != EditorWorkspaceViewCommand::InstantiateAsset &&
            cmd.command != EditorWorkspaceViewCommand::CancelAssetPlacementPreview) {
            CancelAssetPlacementPreview();
        }
        if (cmd.command != EditorWorkspaceViewCommand::None && cmd.command != EditorWorkspaceViewCommand::EditPCGGraph)
            for (auto &document : m_viewModel.pcgDocuments)
                CancelPCGCanvasGesture(document.canvas);
        if (!m_viewModel.selectedColliders.empty() && cmd.command != EditorWorkspaceViewCommand::None &&
            cmd.command != EditorWorkspaceViewCommand::SelectPhysicsColliders &&
            cmd.command != EditorWorkspaceViewCommand::CommitPhysicsColliders &&
            cmd.command != EditorWorkspaceViewCommand::NavigateViewport && cmd.command != EditorWorkspaceViewCommand::AlignViewportToAxis &&
            cmd.command != EditorWorkspaceViewCommand::ChangeViewportProjection &&
            cmd.command != EditorWorkspaceViewCommand::FocusViewportSelection &&
            cmd.command != EditorWorkspaceViewCommand::ChangeTransformTool &&
            cmd.command != EditorWorkspaceViewCommand::ChangeTransformSpace) {
            m_viewModel.selectedColliders.clear();
            ++m_viewModel.colliderSelectionRevision;
        }
        DispatchWorkspaceCommand(cmd);
        RefreshWorldDocumentProjection();
        UpdateWorldBake();
        RefreshSourceDocumentProjection();
        RefreshPrefabDocuments();
    }

    /** @copydoc EditorWorkspaceController::DispatchWorkspaceCommand */
    void EditorWorkspaceController::DispatchWorkspaceCommand(const EditorWorkspaceViewCommandData &cmd) {
        if (cmd.command == EditorWorkspaceViewCommand::WorldLayerAuthoring) {
            const auto result = cmd.worldLayerIntent
                                    ? ApplyWorldLayerIntent(*cmd.worldLayerIntent)
                                    : Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Invalid));
            m_viewModel.worldLayerFailure = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
            RefreshWorldDocumentProjection();
            return;
        }
        if (ProcessWorldStreamingPreviewCommand(cmd) || ProcessWorldBakeCommand(cmd) || ProcessWorldCompositionCommand(cmd) ||
            ProcessWorldSourceCommand(cmd) || ProcessPrefabDocumentCommand(cmd) || ProcessWorkspaceDocumentCommand(cmd) ||
            ProcessSequenceDocumentCommand(cmd) || ProcessSourceDocumentCommand(cmd) || ProcessPCGDocumentCommand(cmd) ||
            ProcessContentBrowserCommand(cmd) || ProcessActivePanelCommand(cmd) || ProcessLayoutCommand(cmd))
            return;
        if (m_activePrefabDocument) {
            ProcessFocusedPrefabCommand(cmd);
            return;
        }
        static_cast<void>(ProcessDocumentCommand(cmd) || ProcessPlayCommand(cmd) || ProcessCreatePrefabCommand(cmd) ||
                          ProcessPrefabHierarchyCommand(cmd) || ProcessSceneObjectCommand(cmd) || ProcessViewportPickCommand(cmd) ||
                          ProcessViewportCommand(cmd) || ProcessComponentCommand(cmd));
    }

    /** @copydoc EditorWorkspaceController::ApplyWorldLayerIntent */
    Result<PartitionedWorldSnapshotLease> EditorWorkspaceController::ApplyWorldLayerIntent(const WorldLayerOutlinerIntent &intent) {
        using enum WorldLayerOutlinerAction;
        switch (intent.action) {
            case Execute:
                return intent.command ? m_worldDocuments.ExecuteLayerCommand(*intent.command, intent.expected)
                                      : Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Invalid));
            case Undo:
            case Redo:
                return m_worldDocuments.ReplayLayerCommand(intent.action == Redo, intent.expected);
            default:
                return Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Unsupported));
        }
    }

    /** @copydoc EditorWorkspaceController::ProcessFocusedPrefabCommand */
    void EditorWorkspaceController::ProcessFocusedPrefabCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command == DiscardSceneRecovery) {
            static_cast<void>(ProcessDocumentCommand(command));
            return;
        }
        if (command.command == StopPlay || command.command == PausePlay || command.command == ResumePlay || command.command == StepPlay) {
            static_cast<void>(ProcessPlayCommand(command));
            return;
        }
        using enum PrefabDocumentAction;
        PrefabDocumentAction action;
        switch (command.command) {
            case SaveScene:
                action = Save;
                break;
            case UndoScene:
                action = Undo;
                break;
            case RedoScene:
                action = Redo;
                break;
            default:
                return;
        }
        const auto entry = std::ranges::find_if(m_prefabDocuments, [this](const auto &document) {
            return document->Instance() == m_activePrefabDocument;
        });
        if (entry == m_prefabDocuments.end())
            return;
        EditorWorkspaceViewCommandData request;
        request.command = PrefabDocument;
        request.prefabFence = (*entry)->Snapshot().fence;
        request.prefabAction = action;
        static_cast<void>(ProcessPrefabDocumentCommand(request));
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
                m_viewModel.prefabSelection.reset();
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
