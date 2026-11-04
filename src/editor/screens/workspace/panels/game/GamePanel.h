#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <imgui.h>

namespace Horo::Editor {
    /** @brief Persistent document panel presenting the active authored camera or explicitly selected XR left-eye mirror. */
    class GamePanel final : public IWorkspacePanel {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.game"};

        /** @brief Creates a surface with its required localization scope. */
        GamePanel() : IWorkspacePanel(kTextScope) {}

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
        void RequestViewport(float width, float height);
        [[nodiscard]] bool CanDraw(const EditorWorkspaceViewModel &viewModel, const EditorViewportTextureView &texture) const;
        void DrawUnavailable(const ImVec2 &origin, float width, float height, const EditorWorkspaceViewModel &viewModel,
                             const EditorGuiContext &context);
        IEditorViewportRenderer *viewportRenderer_{};
    };
}  // namespace Horo::Editor
