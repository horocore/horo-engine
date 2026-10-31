#include "EditorWorkspaceView.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "terrain/TerrainDocumentHost.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceView::DrawTerrainDocument */
    bool EditorWorkspaceView::DrawTerrainDocument(const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command) const {
        const auto active = model.workspacePanelHost.ActiveDocument();
        const auto found = std::ranges::find_if(model.terrainDocuments, [&](const auto &document) {
            return document->Identity().instance == active;
        });
        if (found == model.terrainDocuments.end())
            return false;
        const auto &panels = m_panelRegistry.GetAllPanels();
        const auto viewport = std::ranges::find_if(panels, [](const auto &panel) {
            return panel->GetId() == "horo.viewport";
        });
        if (viewport == panels.end()) {
            Ui::ErrorText(m_context.localization.Get("editor", "terrain.tools.unavailable").c_str(), m_context.theme.fonts);
            return true;
        }
        GuiLive::InvokeVirtual<&IWorkspacePanel::DrawPanel>(m_context.theme.liveReload, **viewport, ImGui::GetCursorScreenPos(),
                                                            ImGui::GetContentRegionAvail(), model, command, m_context);
        return true;
    }
}  // namespace Horo::Editor
