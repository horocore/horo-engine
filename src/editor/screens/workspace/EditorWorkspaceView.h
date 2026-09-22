#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "WorkspaceSplitterInteraction.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <cstdint>
#include <imgui.h>
#include <string>

namespace Horo::Editor {
    struct GuiContentRegion;

    class EditorWorkspaceView final : public Input::IInputCaptureOwner {
    public:
        struct WorkspaceLayoutGeometry {
            ImVec2 display{};
            float curY{0.0F};
            float leftActivityW{0.0F};
            float rightActivityW{0.0F};
            float hierarchyW{0.0F};
            float inspectorW{0.0F};
            float centerW{0.0F};
            float bottomDockW{0.0F};
            float availableDockW{0.0F};
            float mainH{0.0F};
            float contentH{0.0F};
            float activityBarH{0.0F};
        };

        struct ActivityBarOptions {
            WorkspaceDockArea area;
            bool indicatorOnRight;
            bool allowDragSources;
        };

        struct ActivityBarGeometry {
            float cellX;
            float contentY;
            float cellWidth;
            float cellHeight;
            float cellGap;
            ImDrawList *drawList;
        };

        struct ActivityBarGroupParams {
            std::size_t groupIndex{0};
            const ActivityBarGroup &group;
            float groupTop{0.0F};
            float groupBottom{0.0F};
            ImVec2 pos{};
            ImVec2 size{};
            const ActivityBarGeometry &geometry;
            const ActivityBarOptions &options;
            bool draggingActivityItem{false};
        };

        EditorWorkspaceView(const EditorGuiContext &context, const WorkspacePanelRegistry &panelRegistry, std::uintptr_t logoTexture,
                            Input::InputRouter &inputRouter, Input::InputContextToken &workspaceInputContext);

        void Draw(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand,
                  const GuiContentRegion &contentRegion);
        void OnInputCaptureCancelled(Input::CaptureCancellationReason reason) noexcept override;

    private:
        const EditorGuiContext &m_context;
        const WorkspacePanelRegistry &m_panelRegistry;
        std::uintptr_t m_logoTexture;
        Input::InputRouter &m_inputRouter;
        Input::InputContextToken &m_workspaceInputContext;
        mutable WorkspaceSplitterInteraction m_splitterInteraction;
        mutable Input::InputContextToken m_panelDragContext;
        mutable Input::PointerCaptureToken m_panelDragCapture;
        std::string m_panelDragCandidateId;

        [[nodiscard]] bool EnsurePanelDragCapture();
        [[nodiscard]] bool PanelDragEligible() const noexcept;

        /**
         * @brief Draws the shared activity/document panel drag source payload.
         * @param panelId Stable workspace panel identifier used as the payload.
         * @param panel Panel supplying the drag-preview label.
         */
        void DrawActivityPanelDragSource(const std::string &panelId, const std::shared_ptr<IWorkspacePanel> &panel);

        void DrawMenuBar(const ImVec2 &display, const EditorWorkspaceViewModel &viewModel,
                         EditorWorkspaceViewCommandData &outCommand) const;

        void DrawToolbar(const ImVec2 &pos, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                         EditorWorkspaceViewCommandData &outCommand);
        void DrawDocumentRail(const ImVec2 &pos, const ImVec2 &size, float centerY, float minimumX, float maximumX,
                              const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand);
        void DrawDocumentRailItem(const std::string &panelId, const std::shared_ptr<IWorkspacePanel> &panel, float tabX, float centerY,
                                  const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand);

        void DrawRecoveryBar(const ImVec2 &pos, const ImVec2 &size, EditorWorkspaceViewCommandData &outCommand) const;

        void DrawExternalConflictBar(const ImVec2 &pos, const ImVec2 &size, EditorWorkspaceViewCommandData &outCommand) const;

        void DrawDockArea(WorkspaceDockArea area, const char *windowId, const ImVec2 &pos, const ImVec2 &size,
                          std::string_view activePanelId, const EditorWorkspaceViewModel &viewModel,
                          EditorWorkspaceViewCommandData &outCommand);
        void DrawMiddleAndBottomDocks(const WorkspaceLayoutGeometry &geo, const EditorWorkspaceViewModel &viewModel,
                                      EditorWorkspaceViewCommandData &outCommand);
        void DrawWorkspaceDropTarget(const char *targetNodeId, const char *id, const ImVec2 &position, const ImVec2 &size,
                                     WorkspacePanelHost::DropKind kind, EditorWorkspaceViewCommandData &outCommand) const;

        void DrawActivityBar(const ImVec2 &pos, const ImVec2 &size, const WorkspacePanelRegistry &registry,
                             const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand,
                             ActivityBarOptions options);
        void DrawActivityBarGroup(const ActivityBarGroupParams &params, const EditorWorkspaceViewModel &viewModel,
                                  EditorWorkspaceViewCommandData &outCommand);
        bool DrawActivityDropSlot(ActivityBarSlot slot, float y, bool draggingActivityItem, const ActivityBarGeometry &geometry,
                                  EditorWorkspaceViewCommandData &outCommand) const;
        float DrawActivityItem(const std::string &panelId, float y, const ActivityBarGeometry &geometry,
                               const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand,
                               ActivityBarOptions options, const std::shared_ptr<IWorkspacePanel> &panel);
    };
}  // namespace Horo::Editor
