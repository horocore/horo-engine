#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserPane.h"

#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/grid/AssetBrowserGrid.h"

namespace Horo::Editor {
    /** @copydoc AssetBrowserPane::Attach */
    void AssetBrowserPane::Attach(IEditorGuiRenderer *guiRenderer) noexcept {
        m_cardRenderer.Attach(guiRenderer);
    }

    /** @copydoc AssetBrowserPane::Detach */
    void AssetBrowserPane::Detach() noexcept {
        m_cardRenderer.Detach();
    }

    /** @copydoc AssetBrowserPane::Reveal */
    void AssetBrowserPane::Reveal(const std::string_view absolutePath) {
        m_interactionSession.RevealSelection(absolutePath);
    }

    /** @copydoc AssetBrowserPane::Draw */
    void AssetBrowserPane::Draw(const ImVec2 &contentOrigin, const float contentWidth, const EditorWorkspaceViewModel &viewModel,
                                EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        DrawAssetBrowserGrid(contentOrigin, contentWidth, viewModel, command, context, m_interactionSession, m_cardRenderer);
    }
}  // namespace Horo::Editor
