#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/hierarchy/HierarchyPanel.h"

#include <cstring>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        constexpr const char *PlacementPayload = "HORO_PREFAB_PLACEMENT";
        static_assert(std::is_trivially_copyable_v<PrefabHierarchyCommand>);

        /** @brief Copies one exact value-only gesture; the controller still validates every identity and revision. */
        void AcceptPlacement(const std::optional<SceneObjectId> parent, EditorWorkspaceViewCommandData &command) {
            if (!ImGui::BeginDragDropTarget())
                return;
            if (const auto *payload = ImGui::AcceptDragDropPayload(PlacementPayload);
                payload != nullptr && payload->Data != nullptr && payload->DataSize == static_cast<int>(sizeof(PrefabHierarchyCommand))) {
                PrefabHierarchyCommand request;
                std::memcpy(&request, payload->Data, sizeof(request));
                request.action = PrefabHierarchyAction::ReparentInstance;
                request.destinationParent = parent;
                command.command = EditorWorkspaceViewCommand::PrefabHierarchy;
                command.prefabHierarchy = request;
            }
            ImGui::EndDragDropTarget();
        }
    }  // namespace

    /** @copydoc HierarchyPanel::DrawPrefabDragDrop */
    void HierarchyPanel::DrawPrefabDragDrop(const HierarchyNodeId id, EditorWorkspaceViewCommandData &command,
                                            const EditorGuiContext &context) const {
        const auto *target = editSession_.PrefabTarget(id);
        if (target == nullptr) {
            AcceptPlacement(SceneObjectId{id}, command);
            return;
        }
        if (target->member || !ImGui::BeginDragDropSource())
            return;
        const auto source = editSession_.PrefabCommand(id, PrefabHierarchyAction::ReparentInstance);
        if (source.prefabHierarchy) {
            ImGui::SetDragDropPayload(PlacementPayload, &*source.prefabHierarchy, sizeof(PrefabHierarchyCommand));
            Ui::Hint(context.localization.Get("editor", "workspace.hierarchy.prefab.instance").c_str(), context.theme.fonts);
        }
        ImGui::EndDragDropSource();
    }

    /** @copydoc HierarchyPanel::AcceptPrefabRootDrop */
    void HierarchyPanel::AcceptPrefabRootDrop(EditorWorkspaceViewCommandData &command) const {
        AcceptPlacement(std::nullopt, command);
    }
}  // namespace Horo::Editor
