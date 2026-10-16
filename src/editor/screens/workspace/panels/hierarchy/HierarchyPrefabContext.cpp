#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/hierarchy/HierarchyPanel.h"

namespace Horo::Editor {
    /** @copydoc HierarchyPanel::DrawPrefabContext */
    void HierarchyPanel::DrawPrefabContext(const HierarchyNodeId id, const bool eligible, EditorWorkspaceViewCommandData &command,
                                           const EditorGuiContext &context) {
        const auto *target = editSession_.PrefabTarget(id);
        if (target == nullptr)
            return;
        if (eligible && Ui::ContextMenuItem(context.localization.Get("editor", "workspace.hierarchy.prefab.open_source").c_str(), nullptr,
                                            context.theme.fonts))
            command = editSession_.PrefabCommand(id, PrefabHierarchyAction::OpenSource);
        if (eligible && Ui::ContextMenuItem(context.localization.Get("editor", "workspace.hierarchy.prefab.locate_instance").c_str(),
                                            nullptr, context.theme.fonts))
            command = editSession_.PrefabCommand(id, PrefabHierarchyAction::LocateInstance);
        Ui::ContextMenuSeparator();
        if (eligible && !target->member &&
            Ui::ContextMenuItem(context.localization.Get("editor", "workspace.hierarchy.duplicate").c_str(), nullptr, context.theme.fonts))
            command = editSession_.PrefabCommand(id, PrefabHierarchyAction::DuplicateInstance);
        if (eligible && !target->member &&
            Ui::ContextMenuItem(context.localization.Get("editor", "workspace.hierarchy.delete").c_str(), "Delete", context.theme.fonts,
                                Ui::ContextMenuItemTone::Danger))
            command = editSession_.PrefabCommand(id, PrefabHierarchyAction::DeleteInstance);
    }
}  // namespace Horo::Editor
