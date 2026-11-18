#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        /** @brief Emits one exact override operation from a retained inspector projection. */
        void DrawOverrideOperation(const PrefabHierarchyEntry &entry, const PrefabHierarchyProjection &projection,
                                   EditorWorkspaceViewCommandData &command, const EditorGuiContext &context,
                                   const PrefabHierarchyRevert selection, const PrefabHierarchyAction action, const char *label,
                                   const bool enabled) {
            if (Ui::Button({.label = context.localization.Get("editor", label).c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = enabled,
                            .font = context.theme.fonts.sans,
                            .style = {.width = Ui::StyleWidth::FillAvailable}})) {
                command.command = EditorWorkspaceViewCommand::PrefabHierarchy;
                command.prefabHierarchy = PrefabHierarchyCommand{projection.documentSession,
                                                                 projection.publication,
                                                                 projection.documentRevision,
                                                                 entry.target,
                                                                 action,
                                                                 {},
                                                                 selection};
            }
        }

        /** @brief Draws typed prefab ownership actions from immutable projection evidence without panel state. */
        void DrawPrefabOwnershipActions(const PrefabHierarchyEntry &entry, const PrefabHierarchyProjection &projection,
                                        EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
            using enum PrefabHierarchyAction;
            for (const auto action : {OpenSource, LocateInstance}) {
                const auto key =
                    action == OpenSource ? "workspace.hierarchy.prefab.open_source" : "workspace.hierarchy.prefab.locate_instance";
                if (Ui::Button({.label = context.localization.Get("editor", key).c_str(),
                                .variant = Ui::ButtonVariant::Secondary,
                                .font = context.theme.fonts.sans,
                                .style = {.width = Ui::StyleWidth::FillAvailable}})) {
                    command.command = EditorWorkspaceViewCommand::PrefabHierarchy;
                    command.prefabHierarchy = PrefabHierarchyCommand{projection.documentSession, projection.publication,
                                                                     projection.documentRevision, entry.target, action};
                }
            }
        }
    }  // namespace

    /** @copydoc InspectorPanel::DrawPrefabSelection */
    void InspectorPanel::DrawPrefabSelection(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                             const EditorGuiContext &context) {
        if (!viewModel.prefabHierarchy || !viewModel.prefabSelection)
            return;
        if (m_prefabPresentation != viewModel.prefabHierarchy || m_prefabPresentedTarget != viewModel.prefabSelection) {
            m_prefabEntry = nullptr;
            m_prefabPresentation = viewModel.prefabHierarchy;
            m_prefabPresentedTarget = viewModel.prefabSelection;
            m_prefabEntry = m_prefabPresentation->Find(*m_prefabPresentedTarget);
        }
        const auto *entry = m_prefabEntry;
        if (entry == nullptr)
            return;
        Ui::Card card({.id = "prefab.source.ownership"});
        card.DrawTitleBar(
            {.id = "title",
             .title = context.localization
                          .Get("editor", entry->target.member ? "workspace.hierarchy.prefab.member" : "workspace.hierarchy.prefab.instance")
                          .c_str(),
             .fonts = context.theme.fonts});
        if (!card.BeginBody())
            return;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, entry->name, Theme::Text(), context.theme.fonts);
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, entry->sourceLabel, Theme::Muted(), context.theme.fonts);
        Ui::Hint(context.localization.Get("editor", "workspace.hierarchy.prefab.protected").c_str(), context.theme.fonts);
        if (entry->hasOverrides)
            Ui::Hint(context.localization.Get("editor", "workspace.hierarchy.prefab.overrides").c_str(), context.theme.fonts);
        if (entry->failure)
            Ui::ErrorText(entry->failure->message.c_str(), context.theme.fonts);
        if (entry->valueInspectionFailure)
            Ui::ErrorText(entry->valueInspectionFailure->message.c_str(), context.theme.fonts);
        const bool canRevert =
            !viewModel.prefabHierarchyUnavailable && !viewModel.sceneExternalConflict && viewModel.playState == EditorPlayState::Idle;
        const auto *root = viewModel.prefabHierarchy->Find({entry->target.instance, std::nullopt});
        const bool canApply = canRevert && viewModel.prefabApplyAvailable && !viewModel.recoveryAvailable && root &&
                              std::ranges::any_of(viewModel.prefabDocuments, [root](const auto &candidate) {
            const auto &source = candidate.source->Data();
            return source.assetId == root->source && !candidate.dirty && !candidate.externalConflict;
        });
        if (viewModel.prefabOverrideError)
            Ui::ErrorText(viewModel.prefabOverrideError->message.c_str(), context.theme.fonts);
        using enum PrefabHierarchyAction;
        using enum PrefabHierarchyRevertScope;
        for (std::uint32_t index = 0; index < entry->valueDiff.size(); ++index) {
            const auto &value = entry->valueDiff[index];
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, value.target, Theme::Text(), context.theme.fonts);
            if (value.originKey)
                Ui::Hint(context.localization.Get("editor", value.originKey).c_str(), context.theme.fonts);
            if (value.stateKey)
                Ui::Hint(context.localization.Get("editor", value.stateKey).c_str(), context.theme.fonts);
            if (value.sourcePresent) {
                Ui::Hint(context.localization.Get("editor", "workspace.prefab.diff.source_value").c_str(), context.theme.fonts);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value.source, Theme::Muted(), context.theme.fonts);
            }
            if (value.effectivePresent) {
                Ui::Hint(context.localization.Get("editor", "workspace.prefab.diff.effective_value").c_str(), context.theme.fonts);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value.effective, value.changed ? Theme::Text() : Theme::Muted(),
                                context.theme.fonts);
            }
            if (value.schemaDefault) {
                Ui::Hint(context.localization.Get("editor", "workspace.prefab.diff.schema_default").c_str(), context.theme.fonts);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, *value.schemaDefault, Theme::Muted(), context.theme.fonts);
            }
            ImGui::PushID(static_cast<int>(index));
            if (value.revertValue) {
                DrawOverrideOperation(*entry, *viewModel.prefabHierarchy, command, context, {Value, index}, RevertOverrides,
                                      "workspace.prefab.diff.revert_value", canRevert);
                DrawOverrideOperation(*entry, *viewModel.prefabHierarchy, command, context, {Value, index}, ApplyOverrides,
                                      "workspace.prefab.diff.apply_value", canApply && !entry->failure);
            }
            if (value.revertComponent) {
                DrawOverrideOperation(*entry, *viewModel.prefabHierarchy, command, context, {Component, index}, RevertOverrides,
                                      "workspace.prefab.diff.revert_component", canRevert);
                DrawOverrideOperation(*entry, *viewModel.prefabHierarchy, command, context, {Component, index}, ApplyOverrides,
                                      "workspace.prefab.diff.apply_component", canApply && !entry->failure);
            }
            ImGui::PopID();
        }
        if (entry->valueDiffIncomplete)
            Ui::Hint(context.localization.Get("editor", "workspace.prefab.diff.omitted").c_str(), context.theme.fonts);
        if (entry->hasOverrides) {
            const auto scope = entry->target.member ? Object : Instance;
            DrawOverrideOperation(*entry, *viewModel.prefabHierarchy, command, context, {scope}, RevertOverrides,
                                  scope == Object ? "workspace.prefab.diff.revert_object" : "workspace.prefab.diff.revert_instance",
                                  canRevert);
            DrawOverrideOperation(*entry, *viewModel.prefabHierarchy, command, context, {scope}, ApplyOverrides,
                                  scope == Object ? "workspace.prefab.diff.apply_object" : "workspace.prefab.diff.apply_instance",
                                  canApply && !entry->failure);
        }
        DrawPrefabOwnershipActions(*entry, *viewModel.prefabHierarchy, command, context);
    }

}  // namespace Horo::Editor
