#pragma once

namespace Horo::Editor {
    struct EditorGuiContext;
    struct AssetBrowserInteractionState;
    struct EditorWorkspaceViewModel;
    struct EditorWorkspaceViewCommandData;

    /**
     * @brief Draws every modal workflow owned by the Asset Browser pane.
     * @param state Cross-frame popup state and edit buffers.
     * @param model Current directory and registered schema choices.
     * @param command Command sink for confirmed operations.
     * @param context Editor GUI services and theme.
     */
    void DrawAssetBrowserDialogs(AssetBrowserInteractionState &state, const EditorWorkspaceViewModel &model,
                                 EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
}  // namespace Horo::Editor
