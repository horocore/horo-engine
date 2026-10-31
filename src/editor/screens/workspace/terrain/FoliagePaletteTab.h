#pragma once
/** @file FoliagePaletteTab.h @brief Actual workspace foliage palette tab over immutable host-captured type assets. */
#include "Horo/Editor/IWorkspacePanel.h"

namespace Horo::Editor {
    /** @brief Presentation only; the controller owns active type/selection and the document owns every placement. */
    class FoliagePaletteTab final : public IWorkspacePanel {
    public:
        static constexpr GuiLocalizationScope kTextScope{"workspace"};
        FoliagePaletteTab() : IWorkspacePanel(kTextScope) {}
        [[nodiscard]] std::string GetId() const override { return "horo.foliage_palette"; }
        [[nodiscard]] std::string GetDisplayName() const override { return "workspace.panel.foliage_palette"; }
        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override { return WorkspaceDockArea::Right; }
        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override { return {}; }
        void OnAttach(PanelContext &) override {}
        void OnDetach() override {}
        void DrawIcon(ImDrawList *draw, const ImVec2 &position, const ImVec2 &size, ImU32 color) override;
        void DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &model,
                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) override;
    private:
        void DrawTypes(const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                       const EditorGuiContext &context);
        void DrawCreation(const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                          const EditorGuiContext &context);
    };
}
