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
    namespace {
        [[nodiscard]] bool TryGetDockArea(const int value, WorkspaceDockArea &area) noexcept {
            using enum WorkspaceDockArea;
            switch (value) {
                case 0:
                    area = Left;
                    return true;
                case 1:
                    area = Right;
                    return true;
                case 2:
                    area = Bottom;
                    return true;
                case 3:
                    area = Document;
                    return true;
                default:
                    return false;
            }
        }

        void NormalizeSideDock(SideDockMode &mode, std::string &fullPanel, std::string &topPanel, std::string &bottomPanel) {
            if (mode != SideDockMode::Split || (!topPanel.empty() && !bottomPanel.empty())) {
                return;
            }

            fullPanel = topPanel.empty() ? std::move(bottomPanel) : std::move(topPanel);
            topPanel.clear();
            bottomPanel.clear();
            mode = SideDockMode::Full;
        }

        void SplitFullSideDock(SideDockMode &mode, std::string &fullPanel, std::string &topPanel, std::string &bottomPanel,
                               const SideDockSlot targetSlot, const std::string_view panelId) {
            const std::string previousFull = std::exchange(fullPanel, {});
            topPanel.clear();
            bottomPanel.clear();
            mode = SideDockMode::Split;
            if (targetSlot == SideDockSlot::Top) {
                topPanel = panelId;
                if (previousFull != panelId)
                    bottomPanel = previousFull;
            } else {
                if (previousFull != panelId)
                    topPanel = previousFull;
                bottomPanel = panelId;
            }
        }

        void ActivateSideDock(SideDockMode &mode, std::string &fullPanel, std::string &topPanel, std::string &bottomPanel,
                              const std::optional<SideDockSlot> targetSlot, const std::string_view panelId,
                              std::vector<std::string> &displacedPanelIds) {
            if (targetSlot.has_value() && !panelId.empty()) {
                if (mode == SideDockMode::Full) {
                    SplitFullSideDock(mode, fullPanel, topPanel, bottomPanel, *targetSlot, panelId);
                    return;
                }

                if (topPanel == panelId)
                    topPanel.clear();
                if (bottomPanel == panelId)
                    bottomPanel.clear();
                std::string &targetPanel = *targetSlot == SideDockSlot::Top ? topPanel : bottomPanel;
                displacedPanelIds.push_back(targetPanel);
                targetPanel = panelId;
                return;
            }

            displacedPanelIds.push_back(fullPanel);
            displacedPanelIds.push_back(topPanel);
            displacedPanelIds.push_back(bottomPanel);
            mode = SideDockMode::Full;
            topPanel.clear();
            bottomPanel.clear();
            fullPanel = panelId;
        }

        void NormalizeBottomDock(EditorWorkspaceViewModel &viewModel) {
            if (viewModel.bottomDockMode != BottomDockMode::Split ||
                (!viewModel.activeBottomLeftPanelId.empty() && !viewModel.activeBottomRightPanelId.empty())) {
                return;
            }

            viewModel.activeBottomPanelId = viewModel.activeBottomLeftPanelId.empty() ? std::move(viewModel.activeBottomRightPanelId)
                                                                                      : std::move(viewModel.activeBottomLeftPanelId);
            viewModel.activeBottomLeftPanelId.clear();
            viewModel.activeBottomRightPanelId.clear();
            viewModel.bottomDockMode = BottomDockMode::Full;
        }

        void NormalizeDocks(EditorWorkspaceViewModel &viewModel) {
            NormalizeSideDock(viewModel.leftDockMode, viewModel.activeLeftPanelId, viewModel.activeLeftTopPanelId,
                              viewModel.activeLeftBottomPanelId);
            NormalizeSideDock(viewModel.rightDockMode, viewModel.activeRightPanelId, viewModel.activeRightTopPanelId,
                              viewModel.activeRightBottomPanelId);
            NormalizeBottomDock(viewModel);
        }

        struct ActivityLayoutRegion {
            WorkspaceDockArea area = WorkspaceDockArea::Document;
            std::optional<SideDockSlot> sideSlot;
            std::optional<BottomDockSlot> bottomSlot;

            friend bool operator==(const ActivityLayoutRegion &, const ActivityLayoutRegion &) = default;
        };

        [[nodiscard]] std::optional<ActivityLayoutRegion> RegionForActivitySlot(const ActivityBarSlot slot) {
            if (slot.rail == ActivityBarRail::DocumentTop && slot.groupIndex == 0) {
                return ActivityLayoutRegion{WorkspaceDockArea::Document, std::nullopt, std::nullopt};
            }
            if (slot.rail == ActivityBarRail::Left) {
                switch (slot.groupIndex) {
                    case 0:
                        return ActivityLayoutRegion{WorkspaceDockArea::Left, SideDockSlot::Top, std::nullopt};
                    case 1:
                        return ActivityLayoutRegion{WorkspaceDockArea::Left, SideDockSlot::Bottom, std::nullopt};
                    case 2:
                        return ActivityLayoutRegion{WorkspaceDockArea::Bottom, std::nullopt, BottomDockSlot::Left};
                    default:
                        return std::nullopt;
                }
            }
            if (slot.rail == ActivityBarRail::Right) {
                switch (slot.groupIndex) {
                    case 0:
                        return ActivityLayoutRegion{WorkspaceDockArea::Right, SideDockSlot::Top, std::nullopt};
                    case 1:
                        return ActivityLayoutRegion{WorkspaceDockArea::Right, SideDockSlot::Bottom, std::nullopt};
                    case 2:
                        return ActivityLayoutRegion{WorkspaceDockArea::Bottom, std::nullopt, BottomDockSlot::Right};
                    default:
                        return std::nullopt;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] bool IsPanelActiveInRegion(const EditorWorkspaceViewModel &viewModel, const std::string_view panelId,
                                                 const ActivityLayoutRegion &region) {
            switch (region.area) {
                case WorkspaceDockArea::Left:
                    if (viewModel.leftDockMode == SideDockMode::Full) {
                        return viewModel.activeLeftPanelId == panelId;
                    }
                    return region.sideSlot == SideDockSlot::Top ? viewModel.activeLeftTopPanelId == panelId
                                                                : viewModel.activeLeftBottomPanelId == panelId;
                case WorkspaceDockArea::Right:
                    if (viewModel.rightDockMode == SideDockMode::Full) {
                        return viewModel.activeRightPanelId == panelId;
                    }
                    return region.sideSlot == SideDockSlot::Top ? viewModel.activeRightTopPanelId == panelId
                                                                : viewModel.activeRightBottomPanelId == panelId;
                case WorkspaceDockArea::Bottom:
                    if (viewModel.bottomDockMode == BottomDockMode::Full) {
                        return viewModel.activeBottomPanelId == panelId;
                    }
                    return region.bottomSlot == BottomDockSlot::Left ? viewModel.activeBottomLeftPanelId == panelId
                                                                     : viewModel.activeBottomRightPanelId == panelId;
                case WorkspaceDockArea::Document:
                    return viewModel.activeDocumentPanelId == panelId;
            }
            return false;
        }

        [[nodiscard]] EditorWorkspaceViewCommandData MakeRegionActivationCommand(const std::string_view panelId,
                                                                                 const ActivityLayoutRegion &region) {
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
            command.targetIndex = static_cast<int>(region.area);
            command.stringPayload = std::string(panelId);
            command.sideDockSlot = region.sideSlot;
            command.bottomDockSlot = region.bottomSlot;
            return command;
        }
    }  // namespace

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
                if (cmd.primitivePayload.has_value()) {
                    HandleCreatePrimitive(*cmd.primitivePayload, cmd.objectPayload);
                }
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
                    if (const Result<void> selected = m_selection.SetObjects(request.objects, request.primary); selected.HasError()) {
                        LOG_ERROR("editor.selection", "Select object failed: %s", selected.ErrorValue().message.c_str());
                    }

                    RefreshSelectionProjection();
                }
                break;
            default:
                return false;
        }
        return true;
    }

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
                if (cmd.transformToolPayload.has_value()) {
                    m_viewModel.activeTransformTool = *cmd.transformToolPayload;
                }
                break;
            case EditorWorkspaceViewCommand::ChangeTransformSpace:
                if (cmd.transformSpacePayload.has_value()) {
                    m_viewModel.activeTransformSpace = *cmd.transformSpacePayload;
                }
                break;
            default:
                return false;
        }
        return true;
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

    bool EditorWorkspaceController::ProcessObjectPropertyCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::UpdateObjectName:
                if (cmd.objectPayload.has_value() && cmd.stringPayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    RenameSceneObjectCommand{*cmd.objectPayload, *cmd.stringPayload}),
                                                "Rename object");
                }
                break;
            case EditorWorkspaceViewCommand::UpdateCameraComponent:
                if (cmd.objectPayload.has_value() && cmd.cameraPayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    SetSceneObjectCameraCommand{*cmd.objectPayload, *cmd.cameraPayload}),
                                                "Update camera");
                }
                break;
            case EditorWorkspaceViewCommand::UpdateLightComponent:
                if (cmd.objectPayload.has_value() && cmd.lightPayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    SetSceneObjectLightCommand{*cmd.objectPayload, *cmd.lightPayload}),
                                                "Update light");
                }
                break;
            case EditorWorkspaceViewCommand::UpdateTriggerVolumeComponent:
                if (cmd.objectPayload.has_value() && cmd.triggerVolumePayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    SetSceneObjectTriggerVolumeCommand{*cmd.objectPayload, *cmd.triggerVolumePayload}),
                                                "Update trigger volume");
                }
                break;
            case EditorWorkspaceViewCommand::UpdateAudioSourceComponent:
                if (cmd.objectPayload.has_value() && cmd.audioSourcePayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    SetSceneObjectAudioSourceCommand{*cmd.objectPayload, *cmd.audioSourcePayload}),
                                                "Update audio source");
                }
                break;
            case EditorWorkspaceViewCommand::UpdateObjectEditorState:
                if (cmd.objectPayload.has_value() && cmd.editorStatePayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    SetSceneObjectEditorStateCommand{*cmd.objectPayload, *cmd.editorStatePayload}),
                                                "Update object editor state");
                }
                break;
            case EditorWorkspaceViewCommand::AddComponentToObject:
                if (cmd.objectPayload.has_value() && cmd.componentTypePayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    AddSceneObjectComponentCommand{*cmd.objectPayload, *cmd.componentTypePayload}),
                                                "Add component");
                }
                break;
            case EditorWorkspaceViewCommand::RemoveComponentFromObject:
                if (cmd.objectPayload.has_value() && cmd.componentTypePayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    RemoveSceneObjectComponentCommand{*cmd.objectPayload, *cmd.componentTypePayload}),
                                                "Remove component");
                }
                break;
            default:
                return false;
        }
        return true;
    }

    bool EditorWorkspaceController::ProcessBehaviorCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::AttachBehaviorToObject:
                if (cmd.objectPayload.has_value() && cmd.behaviorTypePayload.has_value()) {
                    const Gameplay::BehaviorRegistration *registration =
                        m_gameplayRegistry != nullptr ? m_gameplayRegistry->Registry().Find(*cmd.behaviorTypePayload) : nullptr;
                    if (registration == nullptr) {
                        LOG_ERROR("editor.gameplay", "Attach behavior rejected because the descriptor is unavailable.");
                        break;
                    }
                    std::vector<Gameplay::BehaviorField> fields;
                    fields.reserve(registration->descriptor.fields.size());
                    for (const Gameplay::BehaviorFieldDescriptor &field : registration->descriptor.fields)
                        fields.emplace_back(field.name, field.defaultValue);
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    AttachSceneObjectBehaviorCommand{*cmd.objectPayload, registration->descriptor.typeId,
                                                                                     registration->descriptor.schemaVersion, true,
                                                                                     registration->descriptor.allowMultiple,
                                                                                     std::move(fields)}),
                                                "Attach behavior");
                }
                break;
            case EditorWorkspaceViewCommand::UpdateBehaviorOnObject:
                if (cmd.objectPayload.has_value() && cmd.behaviorPayload.has_value())
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    SetSceneObjectBehaviorCommand{*cmd.objectPayload, *cmd.behaviorPayload}),
                                                "Update behavior");
                break;
            case EditorWorkspaceViewCommand::RemoveBehaviorFromObject:
                if (cmd.objectPayload.has_value() && cmd.behaviorInstancePayload.has_value())
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    RemoveSceneObjectBehaviorCommand{*cmd.objectPayload, *cmd.behaviorInstancePayload}),
                                                "Remove behavior");
                break;
            default:
                return false;
        }
        return true;
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

    bool EditorWorkspaceController::ProcessContentBrowserAssetCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::RenameContentBrowserEntry:
                if (cmd.stringPayload.has_value() && cmd.secondaryStringPayload.has_value())
                    RenameContentBrowserEntry(*cmd.stringPayload, *cmd.secondaryStringPayload);
                break;
            case EditorWorkspaceViewCommand::DeleteContentBrowserEntry:
                if (cmd.stringPayload.has_value())
                    DeleteContentBrowserEntry(*cmd.stringPayload);
                break;
            case EditorWorkspaceViewCommand::DuplicateContentBrowserAsset:
                if (cmd.stringPayload.has_value())
                    DuplicateContentBrowserAsset(*cmd.stringPayload);
                break;
            case EditorWorkspaceViewCommand::CopyContentBrowserAsset:
                if (cmd.stringPayload.has_value())
                    SetContentBrowserClipboard(*cmd.stringPayload, ContentBrowserClipboardMode::Copy);
                break;
            case EditorWorkspaceViewCommand::CutContentBrowserAsset:
                if (cmd.stringPayload.has_value())
                    SetContentBrowserClipboard(*cmd.stringPayload, ContentBrowserClipboardMode::Move);
                break;
            case EditorWorkspaceViewCommand::PasteContentBrowserAsset:
                PasteContentBrowserAsset(cmd.stringPayload.value_or(m_viewModel.contentBrowser.absoluteCurrentPath));
                break;
            case EditorWorkspaceViewCommand::TransferContentBrowserAsset:
                if (cmd.contentBrowserTransfer.has_value())
                    TransferContentBrowserAsset(*cmd.contentBrowserTransfer);
                break;
            case EditorWorkspaceViewCommand::CancelContentBrowserClipboard:
                ClearContentBrowserClipboard();
                break;
            case EditorWorkspaceViewCommand::CreateContentBrowserFolder:
                if (cmd.stringPayload.has_value() && cmd.secondaryStringPayload.has_value())
                    CreateContentBrowserFolder(*cmd.stringPayload, *cmd.secondaryStringPayload);
                break;
            default:
                return false;
        }
        return true;
    }

    bool EditorWorkspaceController::ProcessContentBrowserSourceCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::CreateLuaBehavior:
                if (cmd.gameplayBehaviorRequest.has_value()) {
                    if (cmd.gameplayBehaviorRequest->kind != GameplayBehaviorKind::Lua) {
                        m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
                        break;
                    }
                    CreateGameplayBehavior(*cmd.gameplayBehaviorRequest);
                } else {
                    LOG_WARN("editor.asset_actions",
                             "CreateLuaBehavior command received without explicit request details; modal prompt expected.");
                }
                break;
            case EditorWorkspaceViewCommand::CreateNativeBehavior:
                if (cmd.gameplayBehaviorRequest.has_value()) {
                    if (cmd.gameplayBehaviorRequest->kind != GameplayBehaviorKind::Native) {
                        m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
                        break;
                    }
                    CreateGameplayBehavior(*cmd.gameplayBehaviorRequest);
                } else {
                    LOG_WARN("editor.asset_actions",
                             "CreateNativeBehavior command received without explicit request details; modal prompt expected.");
                }
                break;
            case EditorWorkspaceViewCommand::ReimportContentBrowserAsset:
                if (cmd.stringPayload.has_value())
                    ReimportContentBrowserAsset(*cmd.stringPayload);
                break;
            case EditorWorkspaceViewCommand::RevealContentBrowserEntry:
                if (cmd.stringPayload.has_value())
                    RevealContentBrowserEntry(*cmd.stringPayload);
                break;
            case EditorWorkspaceViewCommand::OpenSourceFile:
                if (cmd.sourceOpenRequest.has_value())
                    OpenSourceFile(*cmd.sourceOpenRequest);
                break;
            case EditorWorkspaceViewCommand::OpenDiagnosticSource:
                if (cmd.diagnosticSource.has_value())
                    OpenDiagnosticSource(*cmd.diagnosticSource);
                break;
            default:
                return false;
        }
        return true;
    }

    bool EditorWorkspaceController::IsPanelActive(const std::string_view panelId) const {
        return !panelId.empty() && (panelId == m_viewModel.activeLeftPanelId || panelId == m_viewModel.activeRightPanelId ||
                                    panelId == m_viewModel.activeLeftTopPanelId || panelId == m_viewModel.activeLeftBottomPanelId ||
                                    panelId == m_viewModel.activeRightTopPanelId || panelId == m_viewModel.activeRightBottomPanelId ||
                                    panelId == m_viewModel.activeBottomPanelId || panelId == m_viewModel.activeBottomLeftPanelId ||
                                    panelId == m_viewModel.activeBottomRightPanelId || panelId == m_viewModel.activeDocumentPanelId);
    }

    void EditorWorkspaceController::RemovePanelFromDock(const std::string_view panelId, const WorkspaceDockArea area) {
        using enum WorkspaceDockArea;
        const auto clearIfActive = [panelId](std::string &activePanelId) {
            if (activePanelId == panelId)
                activePanelId.clear();
        };
        switch (area) {
            case Left:
                clearIfActive(m_viewModel.activeLeftPanelId);
                clearIfActive(m_viewModel.activeLeftTopPanelId);
                clearIfActive(m_viewModel.activeLeftBottomPanelId);
                break;
            case Right:
                clearIfActive(m_viewModel.activeRightPanelId);
                clearIfActive(m_viewModel.activeRightTopPanelId);
                clearIfActive(m_viewModel.activeRightBottomPanelId);
                break;
            case Bottom:
                clearIfActive(m_viewModel.activeBottomPanelId);
                clearIfActive(m_viewModel.activeBottomLeftPanelId);
                clearIfActive(m_viewModel.activeBottomRightPanelId);
                break;
            case Document:
                clearIfActive(m_viewModel.activeDocumentPanelId);
                break;
        }
        NormalizeDocks(m_viewModel);
    }

    void EditorWorkspaceController::ActivateBottomPanel(const EditorWorkspaceViewCommandData &cmd,
                                                        std::vector<std::string> &displacedPanelIds) {
        const std::string &panelId = *cmd.stringPayload;
        if (!cmd.bottomDockSlot.has_value() || panelId.empty()) {
            displacedPanelIds.push_back(m_viewModel.activeBottomPanelId);
            displacedPanelIds.push_back(m_viewModel.activeBottomLeftPanelId);
            displacedPanelIds.push_back(m_viewModel.activeBottomRightPanelId);
            m_viewModel.bottomDockMode = BottomDockMode::Full;
            m_viewModel.activeBottomLeftPanelId.clear();
            m_viewModel.activeBottomRightPanelId.clear();
            m_viewModel.activeBottomPanelId = panelId;
            return;
        }

        if (m_viewModel.bottomDockMode == BottomDockMode::Full) {
            const std::string previousFull = std::exchange(m_viewModel.activeBottomPanelId, {});
            m_viewModel.activeBottomLeftPanelId.clear();
            m_viewModel.activeBottomRightPanelId.clear();
            m_viewModel.bottomDockMode = BottomDockMode::Split;
            if (*cmd.bottomDockSlot == BottomDockSlot::Left) {
                m_viewModel.activeBottomLeftPanelId = panelId;
                if (previousFull != panelId)
                    m_viewModel.activeBottomRightPanelId = previousFull;
            } else {
                if (previousFull != panelId)
                    m_viewModel.activeBottomLeftPanelId = previousFull;
                m_viewModel.activeBottomRightPanelId = panelId;
            }
            return;
        }

        if (m_viewModel.activeBottomLeftPanelId == panelId)
            m_viewModel.activeBottomLeftPanelId.clear();
        if (m_viewModel.activeBottomRightPanelId == panelId)
            m_viewModel.activeBottomRightPanelId.clear();
        std::string &targetPanel =
            *cmd.bottomDockSlot == BottomDockSlot::Left ? m_viewModel.activeBottomLeftPanelId : m_viewModel.activeBottomRightPanelId;
        displacedPanelIds.push_back(targetPanel);
        targetPanel = panelId;
    }

    void EditorWorkspaceController::ActivatePanelDock(const WorkspaceDockArea area, const EditorWorkspaceViewCommandData &cmd,
                                                      std::vector<std::string> &displacedPanelIds) {
        using enum WorkspaceDockArea;
        switch (area) {
            case Left:
                ActivateSideDock(m_viewModel.leftDockMode, m_viewModel.activeLeftPanelId, m_viewModel.activeLeftTopPanelId,
                                 m_viewModel.activeLeftBottomPanelId, cmd.sideDockSlot, *cmd.stringPayload, displacedPanelIds);
                break;
            case Right:
                ActivateSideDock(m_viewModel.rightDockMode, m_viewModel.activeRightPanelId, m_viewModel.activeRightTopPanelId,
                                 m_viewModel.activeRightBottomPanelId, cmd.sideDockSlot, *cmd.stringPayload, displacedPanelIds);
                break;
            case Bottom:
                ActivateBottomPanel(cmd, displacedPanelIds);
                break;
            case Document:
                displacedPanelIds.push_back(m_viewModel.activeDocumentPanelId);
                m_viewModel.activeDocumentPanelId = *cmd.stringPayload;
                break;
        }
    }

    void EditorWorkspaceController::SyncPanelHost(const WorkspaceDockArea area, const std::string_view panelId) {
        if (panelId.empty())
            return;
        using enum WorkspaceDockArea;
        const char *stackId{};
        switch (area) {
            case Left:
                stackId = "workspace.left";
                break;
            case Document:
                stackId = "workspace.document";
                break;
            case Right:
                stackId = "workspace.right";
                break;
            case Bottom:
                return;
        }
        if (const auto activated = m_viewModel.workspacePanelHost.SetActiveTab(stackId, panelId); !activated.Succeeded())
            static_cast<void>(m_viewModel.workspacePanelHost.DockPanel(panelId, stackId, WorkspacePanelHost::DropKind::TabCenter));
    }

    void EditorWorkspaceController::PublishPanelActivation(const WorkspaceDockArea area, const std::string_view panelId,
                                                           const bool panelWasActive, const std::vector<std::string> &displacedPanelIds) {
        for (const std::string &displacedPanelId : displacedPanelIds) {
            if (displacedPanelId.empty() || displacedPanelId == panelId)
                continue;
            LOG_INFO("editor.workspace", "Panel closed: '%s'", displacedPanelId.c_str());
            m_dataBus.Publish(WorkspacePanelClosedEvent{displacedPanelId, area});
        }
        if (!panelWasActive && !panelId.empty()) {
            LOG_INFO("editor.workspace", "Panel opened: '%s'", std::string{panelId}.c_str());
            m_dataBus.Publish(WorkspacePanelOpenedEvent{std::string{panelId}, area});
        }
    }

    void EditorWorkspaceController::MovePanelActivitySlot(const EditorWorkspaceViewCommandData &cmd) {
        if (cmd.stringPayload->empty() || !cmd.activityBarSlot.has_value())
            return;
        if (const auto previousSlot = m_viewModel.activityBarLayout.FindSlot(*cmd.stringPayload); previousSlot.has_value()) {
            const auto result = m_viewModel.activityBarLayout.Move(*cmd.stringPayload, *cmd.activityBarSlot);
            if (const auto resultingSlot = m_viewModel.activityBarLayout.FindSlot(*cmd.stringPayload);
                result.Succeeded() && result.code != ActivityBarLayoutOperationCode::NoOp && resultingSlot.has_value()) {
                m_dataBus.Publish(ActivityBarItemReorderedEvent{*cmd.stringPayload, *previousSlot, *resultingSlot});
            }
        }
    }

    bool EditorWorkspaceController::ProcessActivePanelCommand(const EditorWorkspaceViewCommandData &cmd) {
        if (cmd.command != EditorWorkspaceViewCommand::ChangeActivePanel)
            return false;
        if (!cmd.targetIndex.has_value() || !cmd.stringPayload.has_value())
            return true;

        WorkspaceDockArea area{};
        if (!TryGetDockArea(*cmd.targetIndex, area))
            return true;

        const std::string &panelId = *cmd.stringPayload;
        const bool panelWasActive = IsPanelActive(panelId);
        std::vector<std::string> displacedPanelIds;
        if (!panelId.empty()) {
            if (const auto previousPlacement = m_viewModel.panelDockAreas.find(panelId);
                previousPlacement != m_viewModel.panelDockAreas.end() && previousPlacement->second != area) {
                RemovePanelFromDock(panelId, previousPlacement->second);
            }
            m_viewModel.panelDockAreas[panelId] = area;
        }

        ActivatePanelDock(area, cmd, displacedPanelIds);
        SyncPanelHost(area, panelId);
        PublishPanelActivation(area, panelId, panelWasActive, displacedPanelIds);
        MovePanelActivitySlot(cmd);
        return true;
    }

    bool EditorWorkspaceController::ProcessLayoutCommand(const EditorWorkspaceViewCommandData &cmd) {
        if (cmd.command == EditorWorkspaceViewCommand::ReorderActivityBarItem) {
            ReorderActivityBarItem(cmd);
            return true;
        }
        if (cmd.command == EditorWorkspaceViewCommand::DockWorkspacePanel) {
            DockWorkspacePanel(cmd);
            return true;
        }
        if (cmd.command == EditorWorkspaceViewCommand::ResizePanel) {
            ResizeWorkspacePanel(cmd);
            return true;
        }
        return false;
    }

    void EditorWorkspaceController::ReorderActivityBarItem(const EditorWorkspaceViewCommandData &cmd) {
        if (!cmd.stringPayload.has_value() || !cmd.activityBarSlot.has_value())
            return;
        const std::optional<ActivityBarSlot> previousSlot = m_viewModel.activityBarLayout.FindSlot(*cmd.stringPayload);
        if (!previousSlot.has_value())
            return;

        const std::optional<ActivityLayoutRegion> previousRegion = RegionForActivitySlot(*previousSlot);
        const std::optional<ActivityLayoutRegion> targetRegion = RegionForActivitySlot(*cmd.activityBarSlot);
        const bool activePanelChangesRegion = previousRegion.has_value() && targetRegion.has_value() && *previousRegion != *targetRegion &&
                                              IsPanelActiveInRegion(m_viewModel, *cmd.stringPayload, *previousRegion);
        if (const ActivityBarLayoutOperationResult result = m_viewModel.activityBarLayout.Move(*cmd.stringPayload, *cmd.activityBarSlot);
            !result.Succeeded() || result.code == ActivityBarLayoutOperationCode::NoOp) {
            return;
        }
        const std::optional<ActivityBarSlot> resultingSlot = m_viewModel.activityBarLayout.FindSlot(*cmd.stringPayload);
        if (!resultingSlot.has_value())
            return;

        if (activePanelChangesRegion) {
            const std::string sourceFallback{m_viewModel.activityBarLayout.ItemAt(previousSlot->rail, previousSlot->groupIndex, 0)};
            ProcessCommand(MakeRegionActivationCommand(*cmd.stringPayload, *targetRegion));
            if (!sourceFallback.empty())
                ProcessCommand(MakeRegionActivationCommand(sourceFallback, *previousRegion));
            NormalizeDocks(m_viewModel);
        } else if (targetRegion.has_value()) {
            m_viewModel.panelDockAreas[*cmd.stringPayload] = targetRegion->area;
        }
        m_dataBus.Publish(ActivityBarItemReorderedEvent{*cmd.stringPayload, *previousSlot, *resultingSlot});
    }

    void EditorWorkspaceController::DockWorkspacePanel(const EditorWorkspaceViewCommandData &cmd) {
        if (!cmd.stringPayload.has_value() || !cmd.workspaceDropTarget.has_value())
            return;
        const auto &target = *cmd.workspaceDropTarget;
        if (const auto result = m_viewModel.workspacePanelHost.DockPanel(*cmd.stringPayload, target.targetNodeId, target.kind);
            result.Succeeded()) {
            m_dataBus.Publish(WorkspacePanelDockedEvent{*cmd.stringPayload, target.targetNodeId, target.kind});
        }
    }

    void EditorWorkspaceController::ResizeWorkspacePanel(const EditorWorkspaceViewCommandData &cmd) {
        if (!cmd.targetIndex.has_value() || !cmd.floatPayload.has_value())
            return;
        WorkspaceDockArea area{};
        if (!TryGetDockArea(*cmd.targetIndex, area))
            return;

        using enum WorkspaceDockArea;
        if (area == Left)
            m_viewModel.leftPanelWidth = *cmd.floatPayload;
        else if (area == Right)
            m_viewModel.rightPanelWidth = *cmd.floatPayload;
        else if (area == Bottom)
            m_viewModel.bottomPanelHeight = *cmd.floatPayload;

        if (cmd.layoutPayload.has_value())
            m_dataBus.Publish(WorkspaceLayoutChangedEvent{area, *cmd.layoutPayload});
    }
}  // namespace Horo::Editor
