#include "component_fields/definitions/CameraFields.h"
#include "component_fields/definitions/LightFields.h"
#include "component_fields/definitions/TriggerVolumeFields.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanelInternal.h"

namespace Horo::Editor {
    using namespace InspectorPanelDetail;

    void InspectorPanel::DrawComponentEditors(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                              EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        if (object.components.camera.has_value()) {
            const InspectorCameraEdit cameraEdit = DrawCameraWidgets(context);
            ApplyCameraEdit(cameraEdit, object, command);
            DrawValidationMessageIfInvalid(m_editSession.IsCameraValid(),
                                           InspectorPanel::kTextScope.Text(context.localization, "inspector.camera_invalid"),
                                           context.theme.fonts, 8.0F);
        }
        if (object.components.light.has_value()) {
            const InspectorLightEdit lightEdit = DrawLightWidgets(context);
            ApplyLightEdit(lightEdit, object, command);
            DrawValidationMessageIfInvalid(m_editSession.IsLightValid(),
                                           InspectorPanel::kTextScope.Text(context.localization, "inspector.light_invalid"),
                                           context.theme.fonts, 8.0F);
        }
        if (object.components.triggerVolume.has_value()) {
            const InspectorTriggerVolumeEdit triggerEdit = DrawTriggerVolumeWidgets(context);
            ApplyTriggerVolumeEdit(triggerEdit, object, command);
        }
        if (object.components.audioSource.has_value()) {
            const InspectorAudioSourceEdit audioEdit = DrawAudioSourceWidgets(context);
            ApplyAudioSourceEdit(audioEdit, object, command);
            DrawAudioPreview(object, viewModel, command, context);
            DrawValidationMessageIfInvalid(m_editSession.IsAudioSourceValid(),
                                           InspectorPanel::kTextScope.Text(context.localization, "inspector.audio_source_invalid"),
                                           context.theme.fonts, 8.0F);
        }
        if (object.components.characterController) {
            if (const auto edit = DrawCharacterWidgets(context);
                !TryAdoptComponentRemoval(edit, object, command, ComponentType::CharacterController))
                AdoptCommand(command, m_editSession.ApplyCharacterEdit(edit, object, command.command == EditorWorkspaceViewCommand::None));
            DrawValidationMessageIfInvalid(m_editSession.IsCharacterValid(),
                                           InspectorPanel::kTextScope.Text(context.localization, "inspector.character.invalid"),
                                           context.theme.fonts, 8.0F);
        }
        if (object.components.audioListener.has_value()) {
            const InspectorAudioListenerEdit audioEdit = DrawAudioListenerWidgets(context);
            ApplyAudioListenerEdit(audioEdit, object, command);
            DrawValidationMessageIfInvalid(m_editSession.IsAudioListenerValid(),
                                           InspectorPanel::kTextScope.Text(context.localization, "inspector.audio_listener_invalid"),
                                           context.theme.fonts, 8.0F);
        }
        DrawPhysicsInspector(object, viewModel, command, context);
        DrawBehaviors(object, viewModel, command, context);
    }

    InspectorCameraEdit InspectorPanel::DrawCameraWidgets(const EditorGuiContext &context) {
        InspectorObjectDraft &draft = m_editSession.Draft();
        if (!draft.camera.has_value())
            return {};

        Ui::Card card(Ui::CardProps{.id = "##CameraCard"});
        const ComponentTitleBarResult header =
            DrawComponentTitleBar(card, InspectorPanel::kTextScope.Text(context.localization, "inspector.camera").c_str(),
                                  {.enabled = draft.camera->enabled}, context);
        if (header.resetRequested) {
            const bool enabled = draft.camera->enabled;
            draft.camera = Runtime::CameraComponent{};
            draft.camera->enabled = enabled;
        }
        if (header.toggleEnabledRequested)
            draft.camera->enabled = !draft.camera->enabled;
        const bool cancelRequested = IsInspectorCardEscapeRequested();
        if (!card.BeginBody())
            return {.cancelRequested = cancelRequested,
                    .committed = header.resetRequested || header.toggleEnabledRequested,
                    .removeRequested = header.removeRequested};
        ImGui::BeginDisabled(!draft.camera->enabled);
        const auto fields = InspectorFields::Draw(*draft.camera, context);
        const bool committed = fields.committed || header.resetRequested || header.toggleEnabledRequested;
        ImGui::EndDisabled();
        card.Finish();
        return {.cancelRequested = cancelRequested, .committed = committed, .removeRequested = header.removeRequested};
    }

    InspectorLightEdit InspectorPanel::DrawLightWidgets(const EditorGuiContext &context) {
        InspectorObjectDraft &draft = m_editSession.Draft();
        if (!draft.light.has_value())
            return {};

        Ui::Card card(Ui::CardProps{.id = "##LightCard"});
        const ComponentTitleBarResult header =
            DrawComponentTitleBar(card, InspectorPanel::kTextScope.Text(context.localization, "inspector.light").c_str(),
                                  {.enabled = draft.light->enabled}, context);
        if (header.resetRequested) {
            const bool enabled = draft.light->enabled;
            draft.light = Runtime::LightComponent{};
            draft.light->enabled = enabled;
        }
        if (header.toggleEnabledRequested)
            draft.light->enabled = !draft.light->enabled;
        const bool cancelRequested = IsInspectorCardEscapeRequested();
        if (!card.BeginBody())
            return {.committed = header.resetRequested || header.toggleEnabledRequested,
                    .cancelRequested = cancelRequested,
                    .removeRequested = header.removeRequested};
        ImGui::BeginDisabled(!draft.light->enabled);
        const auto fields = InspectorFields::Draw(*draft.light, context);
        const bool changed = fields.changed;
        const bool committed = fields.committed || header.resetRequested || header.toggleEnabledRequested;
        ImGui::EndDisabled();
        card.Finish();
        return {
            .changed = changed,
            .committed = committed,
            .cancelRequested = cancelRequested,
            .removeRequested = header.removeRequested,
        };
    }

    InspectorTriggerVolumeEdit InspectorPanel::DrawTriggerVolumeWidgets(const EditorGuiContext &context) {
        InspectorObjectDraft &draft = m_editSession.Draft();
        if (!draft.triggerVolume.has_value())
            return {};

        Ui::Card card(Ui::CardProps{.id = "##TriggerVolumeCard"});
        const ComponentTitleBarResult header =
            DrawComponentTitleBar(card, InspectorPanel::kTextScope.Text(context.localization, "inspector.trigger_volume").c_str(),
                                  {.enabled = draft.triggerVolume->enabled}, context);
        if (header.resetRequested) {
            const bool enabled = draft.triggerVolume->enabled;
            draft.triggerVolume = Runtime::TriggerVolumeComponent{};
            draft.triggerVolume->enabled = enabled;
        }
        if (header.toggleEnabledRequested)
            draft.triggerVolume->enabled = !draft.triggerVolume->enabled;
        const bool cancelRequested = IsInspectorCardEscapeRequested();
        if (!card.BeginBody())
            return {.cancelRequested = cancelRequested,
                    .committed = header.resetRequested || header.toggleEnabledRequested,
                    .removeRequested = header.removeRequested};
        ImGui::BeginDisabled(!draft.triggerVolume->enabled);

        const auto fields = InspectorFields::Draw(*draft.triggerVolume, context);

        ImGui::EndDisabled();
        card.Finish();
        return {.cancelRequested = cancelRequested,
                .committed = fields.committed || header.resetRequested || header.toggleEnabledRequested,
                .removeRequested = header.removeRequested};
    }

}  // namespace Horo::Editor
