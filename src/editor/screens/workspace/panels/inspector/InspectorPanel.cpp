#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanelInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <numbers>
#include <utility>
#include <variant>

namespace Horo::Editor {
    using namespace InspectorPanelDetail;

    void InspectorPanel::OnAttach(PanelContext &ctx) {
        m_inputRouter = ctx.inputRouter;
    }

    void InspectorPanel::OnDetach() {
        m_nameInputContext.Reset();
        m_inputRouter = nullptr;
    }

    void InspectorPanel::DrawIcon(ImDrawList *dl, const ImVec2 &pos, const ImVec2 &size, const ImU32 color) {
        const float ox = pos.x + (size.x - 14.0f) * 0.5f;
        const float oy = pos.y + (size.y - 14.0f) * 0.5f;

        // Simple inspector icon (list with details)
        dl->AddRect(ImVec2(ox + 2, oy + 2), ImVec2(ox + 12, oy + 12), color, 0.0f, 0, 1.5f);
        dl->AddLine(ImVec2(ox + 4, oy + 5), ImVec2(ox + 10, oy + 5), color, 1.5f);
        dl->AddLine(ImVec2(ox + 4, oy + 8), ImVec2(ox + 10, oy + 8), color, 1.5f);
    }

    void InspectorPanel::DrawPanel([[maybe_unused]] const ImVec2 &pos, const ImVec2 &size, const EditorWorkspaceViewModel &vm,
                                   EditorWorkspaceViewCommandData &cmd, const EditorGuiContext &ctx) {
        const float tabHeight = 36.0F * Theme::GetActiveTokens().sizes.uiScale;
        const std::array tabNames{
            ctx.localization.Get("editor", "workspace.panel.inspector").c_str(),
            ctx.localization.Get("editor", "workspace.panel.scene").c_str(),
        };
        m_activeTab = Ui::DrawSideDockTabs(tabNames, m_activeTab, ctx.theme.fonts);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0F, 8.0F));
        ImGui::BeginChild("##Content", ImVec2(size.x, size.y - tabHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoSavedSettings);

        if (m_activeTab != 0) {
            Ui::Hint(ctx.localization.Get("editor", "workspace.inspector.scene_unavailable").c_str(), ctx.theme.fonts);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            return;
        }

        std::array<SceneObjectId, 1> fallbackSelection{};
        std::span<const SceneObjectId> selectedObjects = vm.selectedObjects;
        if (selectedObjects.empty() && vm.primarySelection.has_value()) {
            fallbackSelection.front() = *vm.primarySelection;
            selectedObjects = fallbackSelection;
        }
        if (!selectedObjects.empty() && FindSelectedObject(vm) != nullptr) {
            DrawSelection(vm, selectedObjects, cmd, ctx);
            if (selectedObjects.size() == 1) {
                if (const auto *selected = FindSelectedObject(vm); selected != nullptr)
                    DrawAddComponent(*selected, vm, cmd, ctx);
            }
        } else {
            DrawEmptyState(cmd, ctx);
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    void InspectorPanel::DrawSelection(const EditorWorkspaceViewModel &viewModel, const std::span<const SceneObjectId> selectedObjects,
                                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        AdoptCommand(command, m_editSession.BeginSelection(viewModel.objects, selectedObjects, viewModel.primarySelection,
                                                           viewModel.documentRevision));
        const SceneObject *primaryObject = FindSelectedObject(viewModel);
        if (primaryObject == nullptr)
            return;

        if (m_editSession.Draft().selectedObjectCount == 1) {
            const InspectorNameEdit nameEdit = DrawObjectTitleWidgets(*primaryObject, command, context);
            ApplyNameEdit(nameEdit, *primaryObject, command);
            DrawValidationMessageIfInvalid(IsValidSceneObjectName(m_editSession.Draft().name),
                                           context.localization.Get("editor", "workspace.inspector.name_invalid"), context.theme.fonts,
                                           6.0F, 4.0F);
        } else {
            m_nameInputContext.Reset();
            DrawMultiSelectionTitle(m_editSession.Draft().selectedObjectCount, context);
        }

        const InspectorTransformEdit transformEdit = DrawTransformWidgets(context);
        ApplyTransformEdit(transformEdit, command);
        DrawValidationMessageIfInvalid(m_editSession.IsTransformValid(),
                                       context.localization.Get("editor", "workspace.inspector.transform_invalid"), context.theme.fonts,
                                       8.0F);

        if (m_editSession.Draft().selectedObjectCount == 1)
            DrawComponentEditors(*primaryObject, viewModel, command, context);
    }

    void InspectorPanel::DrawAddComponent(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                          EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) const {
        const std::string addComponentLabel =
            "+  " + context.localization.Get("editor", "workspace.inspector.add_component") + "###InspectorAddComponent";
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Accent());
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
        const bool addComponentPressed = Ui::Button({.label = addComponentLabel.c_str(),
                                                     .size = {0.0F, 34.0F},
                                                     .variant = Ui::ButtonVariant::Secondary,
                                                     .font = context.theme.fonts.sans,
                                                     .componentSize = Ui::ComponentSize::Small,
                                                     .style = {.width = Ui::StyleWidth::FillAvailable}});
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (addComponentPressed) {
            ImGui::OpenPopup("AddComponentPopup");
        }

        if (Ui::BeginMenuPopup("AddComponentPopup")) {
            DrawAddComponentMenuItems(object, viewModel, command, context);
            Ui::EndMenuPopup();
        }
    }

    /** @copydoc InspectorPanelDetail::DrawBehaviorField */
    bool InspectorPanelDetail::DrawBehaviorField(Gameplay::BehaviorField &field, const std::span<const char *const, 2> enabledEntries,
                                                 const EditorGuiContext &context) {
        ImGui::PushID(field.name.c_str());
        const bool fieldCommitted = std::visit(BehaviorFieldVisitor{field, enabledEntries, context}, field.value);
        ImGui::PopID();
        return fieldCommitted;
    }

    void InspectorPanel::DrawBehaviors(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) const {
        const std::array<const char *, 2> enabledEntries{
            context.localization.Get("editor", "workspace.value.off").c_str(),
            context.localization.Get("editor", "workspace.value.on").c_str(),
        };
        for (const Gameplay::BehaviorComponent &attached : object.components.behaviors) {
            const auto descriptorIt =
                std::ranges::find(viewModel.availableBehaviors, attached.typeId, &Gameplay::BehaviorDescriptor::typeId);
            const Gameplay::BehaviorDescriptor *descriptor =
                descriptorIt == viewModel.availableBehaviors.end() ? nullptr : std::to_address(descriptorIt);
            ImGui::PushID(static_cast<int>(attached.instanceId.value));
            BehaviorCardResult result = DrawBehaviorCard(attached, descriptor, enabledEntries, context);
            using enum EditorWorkspaceViewCommand;
            if (result.removeRequested && command.command == None) {
                command.command = RemoveBehaviorFromObject;
                command.objectPayload = object.id;
                command.behaviorInstancePayload = attached.instanceId;
                ImGui::PopID();
                continue;
            }
            if (result.committed && command.command == None) {
                command.command = UpdateBehaviorOnObject;
                command.objectPayload = object.id;
                command.behaviorPayload = std::move(result.edited);
            }
            ImGui::PopID();
        }
    }

    void InspectorPanel::DrawMultiSelectionTitle(const std::size_t selectedObjectCount, const EditorGuiContext &context) const {
        ImGui::SetCursorPos({14.0F, ImGui::GetCursorPosY() + 14.0F});
        const std::string label =
            std::format("{} {}", selectedObjectCount, context.localization.Get("editor", "workspace.inspector.objects_selected"));
        {
            Theme::ScopedTextStyle textStyle(context.theme.fonts.sansEmphasis, Theme::TextPx::Title(), Theme::FontPx::SansEmphasis);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0F);
    }

    InspectorNameEdit InspectorPanel::DrawObjectTitleWidgets(const SceneObject &object, EditorWorkspaceViewCommandData &command,
                                                             const EditorGuiContext &context) {
        InspectorObjectDraft &draft = m_editSession.Draft();
        const std::string &staticLabel = context.localization.Get("editor", "workspace.inspector.static");
        const std::string &optionsLabel = context.localization.Get("editor", "workspace.inspector.object_options");
        const ObjectTitleLayout layout = ResolveObjectTitleLayout(staticLabel.c_str(), context.theme.fonts);
        const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const bool nameWasValid = IsValidSceneObjectName(draft.name);
        const Ui::TextEditResult edit =
            Ui::DrawEditableTitle("object_name", draft.name, MaximumSceneObjectNameBytes, context.theme.fonts,
                                  {.leadingIcon = KindIcon(object.kind), .trailingWidth = layout.trailingWidth, .error = !nameWasValid});
        const ImVec2 rowEnd = ImGui::GetCursorScreenPos();
        DrawStaticObjectIndicator(staticLabel.c_str(), context.theme.fonts, layout, rowOrigin, rowWidth);
        DrawObjectOptions(object, command, context, layout, rowOrigin, rowWidth, optionsLabel.c_str());
        ImGui::SetCursorScreenPos({rowEnd.x, rowEnd.y + 5.0F * layout.uiScale});

        if (edit.active && !m_nameInputContext.IsActive() && m_inputRouter != nullptr) {
            m_nameInputContext = m_inputRouter->PushContext(Input::InputContextId{"editor.inspector.object_name"},
                                                            Input::InputContextKind::FocusedGuiWidget);
        } else if (!edit.active) {
            m_nameInputContext.Reset();
        }
        return {
            .cancelled = edit.cancelled,
            .committed = edit.committed,
        };
    }

    InspectorTransformEdit InspectorPanel::DrawTransformWidgets(const EditorGuiContext &context) {
        InspectorObjectDraft &draft = m_editSession.Draft();
        Ui::Float3PropertyEditResult position;
        Ui::Float3PropertyEditResult rotation;
        Ui::Float3PropertyEditResult scale;
        ComponentTitleBarResult header;
        {
            Ui::Card card(Ui::CardProps{.id = "##TransformCard"});
            header = DrawComponentTitleBar(card, context.localization.Get("editor", "workspace.inspector.transform").c_str(),
                                           {.enabled = true, .canReset = true, .canToggleEnabled = false, .canRemove = false}, context);
            if (card.BeginBody()) {
                position = Ui::DrawFloat3PropRow(context.localization.Get("editor", "workspace.inspector.position").c_str(), "position",
                                                 draft.position, context.theme.fonts, 0.05F, draft.mixed.position);
                rotation = Ui::DrawFloat3PropRow(context.localization.Get("editor", "workspace.inspector.rotation").c_str(), "rotation",
                                                 draft.rotationDegrees, context.theme.fonts, 0.25F, draft.mixed.rotation);
                scale = Ui::DrawFloat3PropRow(context.localization.Get("editor", "workspace.inspector.scale").c_str(), "scale", draft.scale,
                                              context.theme.fonts, 0.05F, draft.mixed.scale);
            }
        }

        return {
            .changed = position.changed || rotation.changed || scale.changed,
            .committed = position.committed || rotation.committed || scale.committed,
            .cancelRequested = m_editSession.HasTransformPreview() && ImGui::IsKeyPressed(ImGuiKey_Escape, false),
            .resetRequested = header.resetRequested,
            .changedAxes =
                {
                    .position = position.changedAxes,
                    .rotation = rotation.changedAxes,
                    .scale = scale.changedAxes,
                },
        };
    }

    void InspectorPanel::ApplyNameEdit(const InspectorNameEdit &edit, const SceneObject &object, EditorWorkspaceViewCommandData &command) {
        AdoptCommand(command, m_editSession.ApplyNameEdit(edit, object, command.command == EditorWorkspaceViewCommand::None));
    }

    void InspectorPanel::ApplyTransformEdit(const InspectorTransformEdit &edit, EditorWorkspaceViewCommandData &command) {
        AdoptCommand(command, m_editSession.ApplyTransformEdit(edit, command.command == EditorWorkspaceViewCommand::None));
    }

    void InspectorPanel::ApplyCameraEdit(const InspectorCameraEdit &edit, const SceneObject &object,
                                         EditorWorkspaceViewCommandData &command) const {
        if (TryAdoptComponentRemoval(edit, object, command, ComponentType::Camera))
            return;
        AdoptCommand(command, m_editSession.ApplyCameraEdit(edit, object, command.command == EditorWorkspaceViewCommand::None));
    }

    void InspectorPanel::ApplyLightEdit(const InspectorLightEdit &edit, const SceneObject &object,
                                        EditorWorkspaceViewCommandData &command) {
        if (TryAdoptComponentRemoval(edit, object, command, ComponentType::Light))
            return;
        AdoptCommand(command, m_editSession.ApplyLightEdit(edit, object, command.command == EditorWorkspaceViewCommand::None));
    }

    void InspectorPanel::ApplyTriggerVolumeEdit(const InspectorTriggerVolumeEdit &edit, const SceneObject &object,
                                                EditorWorkspaceViewCommandData &command) const {
        if (TryAdoptComponentRemoval(edit, object, command, ComponentType::TriggerVolume))
            return;
        AdoptCommand(command, m_editSession.ApplyTriggerVolumeEdit(edit, object, command.command == EditorWorkspaceViewCommand::None));
    }

    void InspectorPanel::ApplyAudioSourceEdit(const InspectorAudioSourceEdit &edit, const SceneObject &object,
                                              EditorWorkspaceViewCommandData &command) const {
        if (TryAdoptComponentRemoval(edit, object, command, ComponentType::AudioSource))
            return;
        AdoptCommand(command, m_editSession.ApplyAudioSourceEdit(edit, object, command.command == EditorWorkspaceViewCommand::None));
    }

    void InspectorPanel::DrawEmptyState(EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        AdoptCommand(command, m_editSession.Clear());
        m_nameInputContext.Reset();

        ImGui::SetCursorPosX(14.0F);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 14.0F);
        Ui::Hint(context.localization.Get("editor", "workspace.inspector.empty").c_str(), context.theme.fonts);
    }

    void InspectorPanel::AdoptCommand(EditorWorkspaceViewCommandData &destination, EditorWorkspaceViewCommandData source) {
        if (destination.command != EditorWorkspaceViewCommand::None || source.command == EditorWorkspaceViewCommand::None) {
            return;
        }
        destination = std::move(source);
    }
}  // namespace Horo::Editor
