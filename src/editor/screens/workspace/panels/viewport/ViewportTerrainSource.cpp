#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "ViewportPanel.h"
#include "editor/screens/workspace/terrain/TerrainViewportTool.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc ViewportPanel::ViewportPanel */
    ViewportPanel::ViewportPanel() : IWorkspacePanel(kTextScope), terrainTool_(std::make_unique<TerrainViewportTool>()) {}

    /** @copydoc ViewportPanel::~ViewportPanel */
    ViewportPanel::~ViewportPanel() = default;

    /** @copydoc ViewportPanel::DrawTerrainSource */
    bool ViewportPanel::DrawTerrainSource(ImDrawList &draw, const ViewportSurfaceLayout &layout, const EditorWorkspaceViewModel &model,
                                          EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        const auto active = model.workspacePanelHost.ActiveDocument();
        const auto found = std::ranges::find_if(model.terrainDocuments, [&](const auto &document) {
            return document->Identity().instance == active;
        });
        if (found == model.terrainDocuments.end()) {
            terrainTool_->Cancel();
            return false;
        }
        worldSourceTool_.Cancel();
        if (interaction_.IsActive()) {
            interaction_.CancelContribution();
            static_cast<void>(interaction_.ConsumePendingCancellation(command));
        }
        draw.AddRectFilled(layout.origin, {layout.origin.x + layout.width, layout.origin.y + layout.height}, Theme::U32(Theme::Bg1()));
        // This is an isolated first-sample-origin asset preview, never a containing Scene transform or native Terrain fallback.
        RequestViewportExtent(layout.width, layout.height);
        if (!viewportRenderer_ || !viewportRenderer_->IsReady() || layout.width <= 0 || layout.height <= 0) {
            terrainTool_->Cancel();
            DrawMissingRendererMessage(layout.centerX, layout.origin.y, layout.height, context);
            return true;
        }
        DrawTerrainTool(draw, layout, model, command, context, **found);
        DrawTerrainDocumentError(layout, model, context);
        return true;
    }

    /** @brief Project one exact owner borrow into the typed tool frame and detach at most one released intent. */
    void ViewportPanel::DrawTerrainTool(ImDrawList &draw, const ViewportSurfaceLayout &layout, const EditorWorkspaceViewModel &model,
                                        EditorWorkspaceViewCommandData &command, const EditorGuiContext &context,
                                        const TerrainDocumentHost &document) {
        const bool eligible = ViewportInputEligible() && model.playState == EditorPlayState::Idle;
        const auto *selection = model.terrainToolSelection.get();
        if (selection && selection->document != document.Identity().instance)
            selection = nullptr;
        std::optional<TerrainToolCommand> intent;
        static_cast<void>(
            terrainTool_->Draw({draw,
                                layout.origin,
                                {layout.width, layout.height},
                                model.viewportCamera,
                                viewportRenderer_->ClipDepthRange(),
                                {},
                                document,
                                eligible,
                                selection,
                                model.terrainCookAvailable,
                                model.terrainCookRunning,
                                model.terrainCookCurrent && model.terrainCookDocument == document.Identity().instance,
                                model.terrainAssetCreationAvailable,
                                model.terrainCookEpoch,
                                model.terrainCookDocument == document.Identity().instance ? model.terrainCookOutcome.get() : nullptr},
                               intent, context));
        if (intent) {
            command.command = EditorWorkspaceViewCommand::TerrainTool;
            command.terrainTool = std::make_shared<const TerrainToolCommand>(std::move(*intent));
        }
    }

    /** @brief Keep original source failure state visible without coupling diagnostic layout to tool interaction. */
    void ViewportPanel::DrawTerrainDocumentError(const ViewportSurfaceLayout &layout, const EditorWorkspaceViewModel &model,
                                                 const EditorGuiContext &context) const {
        if (model.terrainDocumentError) {
            const auto &code = model.terrainDocumentError->code.Value();
            const char *key = code == WorkspaceDocumentErrors::DirtyDocument.code.Value() ? "terrain.tools.dirty_close"
                              : code == Assets::AssetSourceErrors::Conflict.code.Value()  ? "terrain.tools.conflict"
                                                                                          : "terrain.tools.failed";
            const auto cursor = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(
                {layout.origin.x + Ui::ScaledLayoutValue(8), layout.origin.y + layout.height - Ui::ScaledLayoutValue(70)});
            Ui::WrappedText(std::max(1.0F, layout.width - Ui::ScaledLayoutValue(16)), context.localization.Get("editor", key), Theme::Err(),
                            context.theme.fonts);
            ImGui::SetCursorScreenPos(cursor);
        }
    }
}  // namespace Horo::Editor
