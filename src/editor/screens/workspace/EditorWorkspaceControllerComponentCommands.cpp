#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <utility>
#include <vector>

namespace Horo::Editor {

    bool EditorWorkspaceController::ProcessObjectPropertyCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::UpdateObjectName:
                if (cmd.objectPayload.has_value() && cmd.stringPayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    RenameSceneObjectCommand{*cmd.objectPayload, *cmd.stringPayload}),
                                                "Rename object");
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
                return ProcessObjectComponentValueCommand(cmd);
        }
        return true;
    }

    bool EditorWorkspaceController::ProcessObjectComponentValueCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
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
}  // namespace Horo::Editor
