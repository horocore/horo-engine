#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "editor/screens/workspace/panels/hierarchy/HierarchyEditSession.h"

#include <array>
#include <optional>

namespace Horo::Editor {
    class HierarchyPanel final : public IWorkspacePanel {
    public:
        HierarchyPanel() = default;

        [[nodiscard]] std::string GetId() const override {
            return "horo.hierarchy";
        }

        [[nodiscard]] std::string GetDisplayName() const override {
            return "workspace.panel.hierarchy";
        }

        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override {
            return WorkspaceDockArea::Left;
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
        struct PanelInteractionState;
        struct RowFrame;
        struct RowControls;
        struct RowActionIcon;

        struct RowDrawLayout {
            float listWidth{0.0F};
            float outerPadding{0.0F};
            float uiScale{1.0F};
        };

        void BeginRename(HierarchyNodeId id);
        [[nodiscard]] PanelInteractionState DrawSearch(float panelWidth, float uiScale, const EditorGuiContext &context);
        void UpdateFocusedInputContext(bool searchActive);
        void HandleRenameShortcut(const PanelInteractionState &interaction);
        [[nodiscard]] bool AcceptRowAssetDrop(HierarchyNodeId nodeId, float normalizedRowY, const ImVec2 &rowMin, const ImVec2 &rowMax,
                                              const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                              ImDrawList &drawList) const;
        [[nodiscard]] bool DrawRows(const std::vector<HierarchyVisibleRow> &rows, const RowDrawLayout &layout,
                                    const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                    const EditorGuiContext &context);
        [[nodiscard]] RowFrame BuildRowFrame(const HierarchyVisibleRow &row, const RowDrawLayout &layout,
                                             const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                             ImDrawList &drawList, const EditorGuiContext &context) const;
        void DrawRowContextMenu(const RowFrame &frame, bool workspaceEligible, bool &pendingDelete, EditorWorkspaceViewCommandData &command,
                                const EditorGuiContext &context);
        [[nodiscard]] RowControls DrawRowControls(const RowFrame &frame, bool workspaceEligible, const EditorGuiContext &context);
        static void DrawRowPresentation(const RowFrame &frame, const RowControls &controls, const EditorGuiContext &context);
        static void DrawRowBackground(const RowFrame &frame, bool hovered);
        static void DrawRowTree(const RowFrame &frame, const RowControls &controls, float centerY);
        static void DrawRowTypeIcon(const RowFrame &frame, const EditorGuiContext &context, float centerY);
        static void DrawRowActionIcon(const RowFrame &frame, float iconSize, const RowActionIcon &action);
        static void DrawRowActions(const RowFrame &frame, const RowControls &controls);
        void ApplyRowInteraction(const RowFrame &frame, const RowControls &controls, bool workspaceEligible,
                                 EditorWorkspaceViewCommandData &command);
        void DrawRowLabel(const RowFrame &frame, EditorWorkspaceViewCommandData &command);

        HierarchyEditSession editSession_;
        std::array<char, 128> searchBuffer_{};
        std::array<char, 129> renameBuffer_{};
        std::optional<HierarchyNodeId> renamingId_;
        bool requestSearchFocus_{false};
        bool requestRenameFocus_{false};
        Input::InputRouter *inputRouter_{nullptr};
        Input::InputContextToken *workspaceInputContext_{nullptr};
        Input::InputContextToken focusedWidgetContext_;
    };
}  // namespace Horo::Editor
