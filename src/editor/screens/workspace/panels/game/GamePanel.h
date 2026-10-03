#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <imgui.h>

namespace Horo::Editor {
    /** @brief Persistent document panel presenting the active play-session camera output. */
    class GamePanel final : public IWorkspacePanel {
    public:
        [[nodiscard]] std::string GetId() const override {
            return "horo.game";
        }

        [[nodiscard]] std::string GetDisplayName() const override {
            return "workspace.panel.game";
        }

        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override {
            return WorkspaceDockArea::Document;
        }

        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override {
            return {};
        }

        void OnAttach(PanelContext &context) override;
        void OnDetach() override;
        void DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, ImU32 color) override;
        void DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) override;

    private:
        IEditorViewportRenderer *viewportRenderer_{};
    };
}  // namespace Horo::Editor
