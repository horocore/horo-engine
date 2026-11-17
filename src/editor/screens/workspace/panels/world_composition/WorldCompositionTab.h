#pragma once

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <array>

namespace Horo::Editor {
    /** @brief Persistent World Composition tab; owns only disposable filter/table/path presentation.
     * The controller owns documents and query publication. No workspace or selection authority is retained here.
     */
    class WorldCompositionTab final : public IWorkspacePanel {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace"};

        /** @brief Creates a surface with its required localization scope. */
        WorldCompositionTab() : IWorkspacePanel(kTextScope) {}

        [[nodiscard]] std::string GetId() const override {
            return "horo.world_composition";
        }

        [[nodiscard]] std::string GetDisplayName() const override {
            return "workspace.panel.world_composition";
        }

        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override {
            return WorkspaceDockArea::Document;
        }

        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override {
            return {};
        }

        void OnAttach(PanelContext &) override {}

        /** @brief Releases cached source leases and disposable presentation on panel teardown. */
        void OnDetach() override;
        void DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, ImU32 color) override;
        void DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) override;

    private:
        /** @brief Projects application-owned bake availability and sends exact revision/operation-fenced intents.
         * @param vm Immutable controller projection. @param command One frame's selected command. @param context Theme/localization. */
        void DrawBakeControls(const EditorWorkspaceViewModel &vm, EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        /** @brief Presents source admission and failure state; returns whether a canonical document is available. */
        [[nodiscard]] bool DrawAdmission(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                         const EditorGuiContext &context);
        /** @brief Emits one close/filter intent from the currently displayed immutable document. */
        void DrawFilters(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                         const EditorGuiContext &context);
        /** @brief Presents cached source rows and the canonical layer outliner. */
        void DrawTables(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                        const EditorGuiContext &context);
        /** @brief Rebuilds tables only after a publication/filter/locale change; draw frames reuse bounded rows. */
        void RefreshRows(const EditorWorkspaceViewModel &viewModel, const EditorGuiContext &context);
        /** @brief Emits explicit cooked-index mount/unmount intents; retains only a disposable path draft. */
        void DrawStreamingMount(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                const EditorGuiContext &context);
        std::string indexPath_;
        std::string path_;
        PartitionedWorldSnapshotLease source_;
        WorldBrowserQuery query_;
        WorldBrowserQuery draft_;
        std::vector<const char *> layerLabels_;
        std::array<Ui::TableColumn, 5> layerColumns_;
        std::array<Ui::TableColumn, 5> cellColumns_;
        ImVec4 textColor_{};
        std::string localeLabel_;
        std::vector<Ui::TableRow> layers_;
        std::vector<Ui::TableRow> cells_;
    };
}  // namespace Horo::Editor
