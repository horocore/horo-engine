#include "editor/screens/workspace/panels/inspector/InspectorPanelInternal.h"

namespace Horo::Editor {
    using namespace InspectorPanelDetail;

    namespace {
        /** @brief Draws the projection and view-size fields of the camera draft. */
        [[nodiscard]] bool DrawCameraProjectionProperties(InspectorObjectDraft &draft, const EditorGuiContext &context) {
            const std::array<const char *, 2> projectionEntries{
                context.localization.Get("editor", "workspace.inspector.camera_projection_perspective").c_str(),
                context.localization.Get("editor", "workspace.inspector.camera_projection_orthographic").c_str(),
            };
            int projection = draft.camera->projection == Runtime::CameraProjection::Perspective ? 0 : 1;
            const bool projectionChanged =
                Ui::DrawComboPropRow(context.localization.Get("editor", "workspace.inspector.camera_projection").c_str(),
                                     "camera_projection", projection, projectionEntries, context.theme.fonts);
            if (projectionChanged)
                draft.camera->projection =
                    projection == 0 ? Runtime::CameraProjection::Perspective : Runtime::CameraProjection::Orthographic;

            bool committed = projectionChanged;
            if (draft.camera->projection == Runtime::CameraProjection::Perspective) {
                const bool fieldOfViewValid = IsValidFieldOfViewDegrees(draft.cameraFieldOfViewDegrees);
                const Ui::PropertyEditResult fieldOfView =
                    Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.camera_field_of_view").c_str(),
                                         "camera_field_of_view", draft.cameraFieldOfViewDegrees, context.theme.fonts,
                                         Ui::FloatPropertyOptions{.speed = 0.25F, .error = !fieldOfViewValid, .format = "%.1f°"});
                if (fieldOfView.changed)
                    draft.camera->verticalFieldOfViewRadians = draft.cameraFieldOfViewDegrees * DegreesToRadians;
                committed = committed || fieldOfView.committed;
            } else {
                const bool orthographicHeightValid =
                    std::isfinite(draft.camera->orthographicHeight) && draft.camera->orthographicHeight > 0.0F;
                const Ui::PropertyEditResult orthographicHeight =
                    Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.camera_orthographic_height").c_str(),
                                         "camera_orthographic_height", draft.camera->orthographicHeight, context.theme.fonts,
                                         Ui::FloatPropertyOptions{.speed = 0.05F, .error = !orthographicHeightValid});
                committed = committed || orthographicHeight.committed;
            }
            return committed;
        }

        /** @brief Draws the near and far clip-plane fields of the camera draft. */
        [[nodiscard]] bool DrawCameraClipProperties(InspectorObjectDraft &draft, const EditorGuiContext &context) {
            const bool nearPlaneValid = std::isfinite(draft.camera->nearPlane) && draft.camera->nearPlane > 0.0F &&
                                        std::isfinite(draft.camera->farPlane) && draft.camera->farPlane > draft.camera->nearPlane;
            const Ui::PropertyEditResult nearPlane =
                Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.camera_near_plane").c_str(),
                                     "camera_near_plane", draft.camera->nearPlane, context.theme.fonts,
                                     Ui::FloatPropertyOptions{.speed = 0.01F, .error = !nearPlaneValid});
            const bool farPlaneValid = std::isfinite(draft.camera->farPlane) && draft.camera->farPlane > draft.camera->nearPlane;
            const Ui::PropertyEditResult farPlane =
                Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.camera_far_plane").c_str(), "camera_far_plane",
                                     draft.camera->farPlane, context.theme.fonts,
                                     Ui::FloatPropertyOptions{.speed = 1.0F, .error = !farPlaneValid});
            return nearPlane.committed || farPlane.committed;
        }

        struct LightWidgetEdits {
            bool changed{false};
            bool committed{false};
        };

        /** @brief Draws the common kind, color, and intensity fields of the light draft. */
        [[nodiscard]] LightWidgetEdits DrawLightBaseProperties(InspectorObjectDraft &draft, const EditorGuiContext &context) {
            const std::array<const char *, 3> kindEntries{
                context.localization.Get("editor", "workspace.inspector.light_kind_directional").c_str(),
                context.localization.Get("editor", "workspace.inspector.light_kind_point").c_str(),
                context.localization.Get("editor", "workspace.inspector.light_kind_spot").c_str(),
            };
            auto kind = static_cast<int>(draft.light->kind);
            const bool kindChanged = Ui::DrawComboPropRow(context.localization.Get("editor", "workspace.inspector.light_kind").c_str(),
                                                          "light_kind", kind, kindEntries, context.theme.fonts);
            if (kindChanged)
                draft.light->kind = static_cast<Runtime::LightKind>(kind);

            std::array color{draft.light->color.x, draft.light->color.y, draft.light->color.z};
            const bool colorValid = std::ranges::all_of(color, [](const float component) {
                return std::isfinite(component);
            });
            const Ui::PropertyEditResult colorEdit =
                Ui::DrawColor3PropRow(context.localization.Get("editor", "workspace.inspector.light_color").c_str(), "light_color", color,
                                      context.theme.fonts, !colorValid);
            if (colorEdit.changed)
                draft.light->color = {color[0], color[1], color[2]};

            const bool intensityValid = std::isfinite(draft.light->intensity) && draft.light->intensity >= 0.0F;
            const Ui::PropertyEditResult intensity =
                Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.light_intensity").c_str(), "light_intensity",
                                     draft.light->intensity, context.theme.fonts,
                                     Ui::FloatPropertyOptions{.speed = 0.05F, .error = !intensityValid});
            return {.changed = kindChanged || colorEdit.changed || intensity.changed,
                    .committed = kindChanged || colorEdit.committed || intensity.committed};
        }

        /** @brief Draws the range field for non-directional lights. */
        [[nodiscard]] LightWidgetEdits DrawLightRangeProperties(InspectorObjectDraft &draft, const EditorGuiContext &context) {
            if (draft.light->kind == Runtime::LightKind::Directional)
                return {};
            const bool rangeValid = std::isfinite(draft.light->range) && draft.light->range >= 0.0F;
            const Ui::PropertyEditResult range =
                Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.light_range").c_str(), "light_range",
                                     draft.light->range, context.theme.fonts,
                                     Ui::FloatPropertyOptions{.speed = 0.1F, .error = !rangeValid});
            return {.changed = range.changed, .committed = range.committed};
        }

        /** @brief Draws the inner and outer cone fields for spot lights. */
        [[nodiscard]] LightWidgetEdits DrawLightSpotProperties(InspectorObjectDraft &draft, const EditorGuiContext &context) {
            if (draft.light->kind != Runtime::LightKind::Spot)
                return {};
            const bool innerValid = std::isfinite(draft.lightInnerConeDegrees) && draft.lightInnerConeDegrees >= 0.0F &&
                                    std::isfinite(draft.lightOuterConeDegrees) &&
                                    draft.lightOuterConeDegrees >= draft.lightInnerConeDegrees;
            const Ui::PropertyEditResult inner =
                Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.light_inner_cone").c_str(), "light_inner_cone",
                                     draft.lightInnerConeDegrees, context.theme.fonts,
                                     Ui::FloatPropertyOptions{.speed = 0.25F, .error = !innerValid, .format = "%.1f°"});
            if (inner.changed)
                draft.light->innerConeRadians = draft.lightInnerConeDegrees * DegreesToRadians;

            const bool outerValid =
                std::isfinite(draft.lightOuterConeDegrees) && draft.lightOuterConeDegrees >= draft.lightInnerConeDegrees;
            const Ui::PropertyEditResult outer =
                Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.light_outer_cone").c_str(), "light_outer_cone",
                                     draft.lightOuterConeDegrees, context.theme.fonts,
                                     Ui::FloatPropertyOptions{.speed = 0.25F, .error = !outerValid, .format = "%.1f°"});
            if (outer.changed)
                draft.light->outerConeRadians = draft.lightOuterConeDegrees * DegreesToRadians;
            return {.changed = inner.changed || outer.changed, .committed = inner.committed || outer.committed};
        }
    }  // namespace

    void InspectorPanel::DrawComponentEditors(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                              EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        if (object.components.camera.has_value()) {
            const InspectorCameraEdit cameraEdit = DrawCameraWidgets(context);
            ApplyCameraEdit(cameraEdit, object, command);
            DrawValidationMessageIfInvalid(m_editSession.IsCameraValid(),
                                           context.localization.Get("editor", "workspace.inspector.camera_invalid"), context.theme.fonts,
                                           8.0F);
        }
        if (object.components.light.has_value()) {
            const InspectorLightEdit lightEdit = DrawLightWidgets(context);
            ApplyLightEdit(lightEdit, object, command);
            DrawValidationMessageIfInvalid(m_editSession.IsLightValid(),
                                           context.localization.Get("editor", "workspace.inspector.light_invalid"), context.theme.fonts,
                                           8.0F);
        }
        if (object.components.triggerVolume.has_value()) {
            const InspectorTriggerVolumeEdit triggerEdit = DrawTriggerVolumeWidgets(context);
            ApplyTriggerVolumeEdit(triggerEdit, object, command);
        }
        if (object.components.audioSource.has_value()) {
            const InspectorAudioSourceEdit audioEdit = DrawAudioSourceWidgets(context);
            ApplyAudioSourceEdit(audioEdit, object, command);
            DrawValidationMessageIfInvalid(m_editSession.IsAudioSourceValid(),
                                           context.localization.Get("editor", "workspace.inspector.audio_source_invalid"),
                                           context.theme.fonts, 8.0F);
        }
        DrawBehaviors(object, viewModel, command, context);
    }

    InspectorCameraEdit InspectorPanel::DrawCameraWidgets(const EditorGuiContext &context) {
        InspectorObjectDraft &draft = m_editSession.Draft();
        if (!draft.camera.has_value())
            return {};

        Ui::Card card(Ui::CardProps{.id = "##CameraCard"});
        const ComponentTitleBarResult header =
            DrawComponentTitleBar(card, context.localization.Get("editor", "workspace.inspector.camera").c_str(),
                                  {.enabled = draft.camera->enabled}, context);
        if (header.resetRequested) {
            const bool enabled = draft.camera->enabled;
            draft.camera = Runtime::CameraComponent{};
            draft.camera->enabled = enabled;
            draft.cameraFieldOfViewDegrees = 60.0F;
        }
        if (header.toggleEnabledRequested)
            draft.camera->enabled = !draft.camera->enabled;
        const bool cancelRequested = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (!card.BeginBody())
            return {.cancelRequested = cancelRequested,
                    .committed = header.resetRequested || header.toggleEnabledRequested,
                    .removeRequested = header.removeRequested};
        ImGui::BeginDisabled(!draft.camera->enabled);
        bool committed = DrawCameraProjectionProperties(draft, context);
        committed = committed || DrawCameraClipProperties(draft, context);
        committed = committed || header.resetRequested || header.toggleEnabledRequested;
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
            DrawComponentTitleBar(card, context.localization.Get("editor", "workspace.inspector.light").c_str(),
                                  {.enabled = draft.light->enabled}, context);
        if (header.resetRequested) {
            const bool enabled = draft.light->enabled;
            draft.light = Runtime::LightComponent{};
            draft.light->enabled = enabled;
            draft.lightInnerConeDegrees = 20.0F;
            draft.lightOuterConeDegrees = 45.0F;
        }
        if (header.toggleEnabledRequested)
            draft.light->enabled = !draft.light->enabled;
        if (!card.BeginBody())
            return {.committed = header.resetRequested || header.toggleEnabledRequested, .removeRequested = header.removeRequested};
        ImGui::BeginDisabled(!draft.light->enabled);
        const LightWidgetEdits base = DrawLightBaseProperties(draft, context);
        const LightWidgetEdits range = DrawLightRangeProperties(draft, context);
        const LightWidgetEdits spot = DrawLightSpotProperties(draft, context);
        const bool changed = base.changed || range.changed || spot.changed;
        const bool committed =
            base.committed || range.committed || spot.committed || header.resetRequested || header.toggleEnabledRequested;
        ImGui::EndDisabled();
        card.Finish();
        return {
            .changed = changed,
            .committed = committed,
            .cancelRequested = m_editSession.HasLightPreview() && ImGui::IsKeyPressed(ImGuiKey_Escape, false),
            .removeRequested = header.removeRequested,
        };
    }

    InspectorTriggerVolumeEdit InspectorPanel::DrawTriggerVolumeWidgets(const EditorGuiContext &context) {
        InspectorObjectDraft &draft = m_editSession.Draft();
        if (!draft.triggerVolume.has_value())
            return {};

        Ui::Card card(Ui::CardProps{.id = "##TriggerVolumeCard"});
        const ComponentTitleBarResult header =
            DrawComponentTitleBar(card, context.localization.Get("editor", "workspace.inspector.trigger_volume").c_str(),
                                  {.enabled = draft.triggerVolume->enabled}, context);
        if (header.resetRequested) {
            const bool enabled = draft.triggerVolume->enabled;
            draft.triggerVolume = Runtime::TriggerVolumeComponent{};
            draft.triggerVolume->enabled = enabled;
        }
        if (header.toggleEnabledRequested)
            draft.triggerVolume->enabled = !draft.triggerVolume->enabled;
        const bool cancelRequested = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (!card.BeginBody())
            return {.cancelRequested = cancelRequested,
                    .committed = header.resetRequested || header.toggleEnabledRequested,
                    .removeRequested = header.removeRequested};
        ImGui::BeginDisabled(!draft.triggerVolume->enabled);

        const std::array<const char *, 4> shapeEntries{
            context.localization.Get("editor", "workspace.inspector.trigger_volume_shape_box").c_str(),
            context.localization.Get("editor", "workspace.inspector.trigger_volume_shape_sphere").c_str(),
            context.localization.Get("editor", "workspace.inspector.trigger_volume_shape_capsule").c_str(),
            context.localization.Get("editor", "workspace.inspector.trigger_volume_shape_plane").c_str(),
        };
        auto shape = static_cast<int>(draft.triggerVolume->shape);
        const bool shapeChanged =
            Ui::DrawComboPropRow(context.localization.Get("editor", "workspace.inspector.trigger_volume_shape").c_str(),
                                 "trigger_volume_shape", shape, shapeEntries, context.theme.fonts);
        if (shapeChanged)
            draft.triggerVolume->shape = static_cast<Runtime::ColliderShapeType>(shape);

        ImGui::EndDisabled();
        card.Finish();
        return {.cancelRequested = cancelRequested,
                .committed = shapeChanged || header.resetRequested || header.toggleEnabledRequested,
                .removeRequested = header.removeRequested};
    }

    InspectorAudioSourceEdit InspectorPanel::DrawAudioSourceWidgets(const EditorGuiContext &context) {
        InspectorObjectDraft &draft = m_editSession.Draft();
        if (!draft.audioSource.has_value())
            return {};

        Ui::Card card(Ui::CardProps{.id = "##AudioSourceCard"});
        const ComponentTitleBarResult header =
            DrawComponentTitleBar(card, context.localization.Get("editor", "workspace.inspector.audio_source").c_str(),
                                  {.enabled = draft.audioSource->enabled}, context);
        if (header.resetRequested) {
            const bool enabled = draft.audioSource->enabled;
            draft.audioSource = Runtime::AudioSourceComponent{};
            draft.audioSource->enabled = enabled;
        }
        if (header.toggleEnabledRequested)
            draft.audioSource->enabled = !draft.audioSource->enabled;
        const bool cancelRequested = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (!card.BeginBody())
            return {.cancelRequested = cancelRequested,
                    .committed = header.resetRequested || header.toggleEnabledRequested,
                    .removeRequested = header.removeRequested};
        ImGui::BeginDisabled(!draft.audioSource->enabled);

        const bool gainValid = std::isfinite(draft.audioSource->playback.gain) && draft.audioSource->playback.gain >= 0.0F;
        const Ui::PropertyEditResult gainEdit =
            Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.audio_source_gain").c_str(), "audio_source_gain",
                                 draft.audioSource->playback.gain, context.theme.fonts,
                                 Ui::FloatPropertyOptions{.speed = 0.01F, .error = !gainValid});

        const bool pitchValid = std::isfinite(draft.audioSource->playback.pitch) && draft.audioSource->playback.pitch > 0.0F &&
                                draft.audioSource->playback.pitch <= 8.0F;
        const Ui::PropertyEditResult pitchEdit =
            Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.inspector.audio_source_pitch").c_str(), "audio_source_pitch",
                                 draft.audioSource->playback.pitch, context.theme.fonts,
                                 Ui::FloatPropertyOptions{.speed = 0.01F, .error = !pitchValid});

        const std::array<const char *, 2> spatialEntries{
            context.localization.Get("editor", "workspace.value.off").c_str(),
            context.localization.Get("editor", "workspace.value.on").c_str(),
        };
        int spatialInt = draft.audioSource->playback.spatialMode == Audio::AudioSpatialMode::ThreeD ? 1 : 0;
        const bool spatialChanged =
            Ui::DrawComboPropRow(context.localization.Get("editor", "workspace.inspector.audio_source_spatial").c_str(),
                                 "audio_source_spatial", spatialInt, spatialEntries, context.theme.fonts);
        if (spatialChanged)
            draft.audioSource->playback.spatialMode = spatialInt != 0 ? Audio::AudioSpatialMode::ThreeD : Audio::AudioSpatialMode::TwoD;

        const bool committed =
            gainEdit.committed || pitchEdit.committed || spatialChanged || header.resetRequested || header.toggleEnabledRequested;
        ImGui::EndDisabled();
        card.Finish();
        return {.cancelRequested = cancelRequested, .committed = committed, .removeRequested = header.removeRequested};
    }
}  // namespace Horo::Editor
