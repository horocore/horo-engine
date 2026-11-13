#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

namespace Horo::Editor {
    namespace {
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
        for (const auto &value : entry->valueDiff) {
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
        }
        if (entry->valueDiffIncomplete)
            Ui::Hint(context.localization.Get("editor", "workspace.prefab.diff.omitted").c_str(), context.theme.fonts);
        DrawPrefabOwnershipActions(*entry, *viewModel.prefabHierarchy, command, context);
    }

}  // namespace Horo::Editor
