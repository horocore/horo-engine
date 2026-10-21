#pragma once
/** @file NavigationTab.h
 * @brief Persistent, presentation-only navigation bake workflow.
 */
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Editor/NavigationBakeWorkspace.h"

namespace Horo::Editor {
    class EditorModalHost;

    /** @brief Closed-by-default bottom panel; the executable retains all accepted work. */
    class NavigationTab final : public IWorkspacePanel {
    public:
        /** @brief Borrows host capabilities and modal composition. @param access Project owner callbacks. @param modals Modal host. */
        NavigationTab(NavigationBakeWorkspaceAccess access, EditorModalHost &modals);

        [[nodiscard]] std::string GetId() const override {
            return "horo.navigation";
        }

        [[nodiscard]] std::string GetDisplayName() const override {
            return "workspace.navigation_bake.title";
        }

        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override {
            return WorkspaceDockArea::Bottom;
        }

        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override {
            return {};
        }

        void OnAttach(PanelContext &context) override;
        void OnDetach() override;
        void DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, ImU32 color) override;
        void DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &model,
                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) override;

    private:
        void DrawChoices(const NavigationBakeCatalog &catalog, const EditorGuiContext &context);
        /** @brief Renders exact authored surface selection without submitting an application command. */
        void DrawSurfaceChoices(const NavigationBakeCatalog &catalog, const EditorGuiContext &context);
        void DrawOperation(const NavigationBakeWorkspaceSnapshot &view, const EditorGuiContext &context) const;
        [[nodiscard]] bool Prepare();
        void StartBake(Navigation::NavigationBakeExecutionMode mode);
        void DrawReloadAction(const NavigationBakeWorkspaceSnapshot &view, const EditorGuiContext &context);
        void DrawOperationActions(const NavigationBakeWorkspaceSnapshot &view, const EditorGuiContext &context);
        void DrawActions(const NavigationBakeWorkspaceSnapshot &view, const EditorGuiContext &context);
        NavigationBakeWorkspaceAccess access_;
        EditorModalHost &modals_;
        std::filesystem::path root_;
        std::vector<Navigation::NavigationBakeSurfaceKey> selected_;
        std::optional<Error> error_;
        bool selectedOnly_{};
        bool prepared_{};
    };
}  // namespace Horo::Editor
