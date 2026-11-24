#pragma once

struct ImVec2;

namespace Horo::Editor {
    class AssetBrowserCardRenderer;
    class AssetBrowserInteractionSession;
    struct EditorGuiContext;
    struct EditorWorkspaceViewCommandData;
    struct EditorWorkspaceViewModel;

    /** @brief Draws the Asset Browser toolbar, status, grid, drag/drop, actions, and dialogs. */
    void DrawAssetBrowserGrid(const ImVec2 &contentOrigin, float contentWidth, const EditorWorkspaceViewModel &viewModel,
                              EditorWorkspaceViewCommandData &command, const EditorGuiContext &context,
                              AssetBrowserInteractionSession &interactionSession, AssetBrowserCardRenderer &cardRenderer);
}  // namespace Horo::Editor
