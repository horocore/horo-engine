#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "editor/screens/workspace/panels/inspector/InspectorEditSession.h"
#include "editor/screens/workspace/panels/inspector/PhysicsInspectorSession.h"

#include <imgui.h>
#include <memory>

namespace Horo::Editor {
    class InspectorContributionView;

    class InspectorPanel final : public IWorkspacePanel {
    public:
        /** @brief Creates an unattached panel; contribution authority is borrowed only on attachment. */
        InspectorPanel();
        /** @brief Releases all cached restricted projections before attached host teardown. */
        ~InspectorPanel() override;

        [[nodiscard]] std::string GetId() const override {
            return "horo.inspector";
        }

        [[nodiscard]] std::string GetDisplayName() const override {
            return "workspace.panel.inspector";
        }

        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override {
            return WorkspaceDockArea::Right;
        }

        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override {
            return {"SceneDocumentChangedEvent", "SelectionChangedEvent"};
        }

        void OnAttach(PanelContext &ctx) override;
        void OnDetach() override;

        void DrawIcon(ImDrawList *dl, const ImVec2 &pos, const ImVec2 &size, ImU32 color) override;

        void DrawPanel(const ImVec2 &pos, const ImVec2 &size, const EditorWorkspaceViewModel &vm, EditorWorkspaceViewCommandData &cmd,
                       const EditorGuiContext &ctx) override;

    private:
        void DrawSelection(const EditorWorkspaceViewModel &viewModel, std::span<const SceneObjectId> selectedObjects,
                           EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        void DrawComponentEditors(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                  EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        void DrawAddComponent(const SceneObject &object, const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                              const EditorGuiContext &context);
        /** @brief Stages explicit Physics creation in the existing add-component route without document publication. */
        void DrawAddPhysicsComponents(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                      const EditorGuiContext &context);

        /** @brief Draw cached portable Physics drafts and submit only explicit complete gestures to the host. */
        void DrawPhysicsInspector(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                  EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        void DrawMultiSelectionTitle(std::size_t selectedObjectCount, const EditorGuiContext &context) const;

        [[nodiscard]] InspectorNameEdit DrawObjectTitleWidgets(const SceneObject &object, EditorWorkspaceViewCommandData &command,
                                                               const EditorGuiContext &context);
        [[nodiscard]] InspectorTransformEdit DrawTransformWidgets(const EditorGuiContext &context);
        [[nodiscard]] InspectorCameraEdit DrawCameraWidgets(const EditorGuiContext &context);
        [[nodiscard]] InspectorLightEdit DrawLightWidgets(const EditorGuiContext &context);
        [[nodiscard]] InspectorTriggerVolumeEdit DrawTriggerVolumeWidgets(const EditorGuiContext &context);
        [[nodiscard]] InspectorAudioSourceEdit DrawAudioSourceWidgets(const EditorGuiContext &context);
        [[nodiscard]] InspectorAudioListenerEdit DrawAudioListenerWidgets(const EditorGuiContext &context);
        void DrawAudioPreview(const SceneObject &object, const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                              const EditorGuiContext &context);
        void DrawBehaviors(const SceneObject &object, const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                           const EditorGuiContext &context) const;

        void ApplyNameEdit(const InspectorNameEdit &edit, const SceneObject &object, EditorWorkspaceViewCommandData &command);
        void ApplyTransformEdit(const InspectorTransformEdit &edit, EditorWorkspaceViewCommandData &command);

        void ApplyCameraEdit(const InspectorCameraEdit &edit, const SceneObject &object, EditorWorkspaceViewCommandData &command);

        void ApplyLightEdit(const InspectorLightEdit &edit, const SceneObject &object, EditorWorkspaceViewCommandData &command);
        void ApplyTriggerVolumeEdit(const InspectorTriggerVolumeEdit &edit, const SceneObject &object,
                                    EditorWorkspaceViewCommandData &command);

        void ApplyAudioSourceEdit(const InspectorAudioSourceEdit &edit, const SceneObject &object, EditorWorkspaceViewCommandData &command);
        void ApplyAudioListenerEdit(const InspectorAudioListenerEdit &edit, const SceneObject &object,
                                    EditorWorkspaceViewCommandData &command);

        /** @brief Presents source ownership and typed navigation for a cached prefab selection without ordinary object editing. */
        void DrawPrefabSelection(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                 const EditorGuiContext &context);
        /** @brief Emits explicit typed source/open and owning-instance navigation intents from the shared controls. */
        void DrawPrefabOwnershipActions(const PrefabHierarchyEntry &entry, const PrefabHierarchyProjection &projection,
                                        EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) const;
        void DrawEmptyState(EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        static void AdoptCommand(EditorWorkspaceViewCommandData &destination, EditorWorkspaceViewCommandData source);

        std::unique_ptr<InspectorContributionView> m_contributions;
        Input::InputRouter *m_inputRouter{nullptr};
        Input::InputContextToken m_nameInputContext;
        InspectorEditSession m_editSession;
        std::shared_ptr<const PrefabHierarchyProjection> m_prefabPresentation;
        std::optional<PrefabHierarchyTarget> m_prefabPresentedTarget;
        const PrefabHierarchyEntry *m_prefabEntry{}; /**< Borrowed only while m_prefabPresentation pins this exact cache. */
        PhysicsInspectorSession m_physicsSession;
        bool m_physicsInputError{};
        int m_activeTab{0};
    };
}  // namespace Horo::Editor
