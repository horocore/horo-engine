#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

namespace Horo::Editor {
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
        DrawPrefabOwnershipActions(*entry, *viewModel.prefabHierarchy, command, context);
    }

    /** @copydoc InspectorPanel::DrawPrefabOwnershipActions */
    void InspectorPanel::DrawPrefabOwnershipActions(const PrefabHierarchyEntry &entry, const PrefabHierarchyProjection &projection,
                                                    EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) const {
        using enum PrefabHierarchyAction;
        for (const auto action : {OpenSource, LocateInstance}) {
            const auto key = action == OpenSource ? "workspace.hierarchy.prefab.open_source" : "workspace.hierarchy.prefab.locate_instance";
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
}  // namespace Horo::Editor
