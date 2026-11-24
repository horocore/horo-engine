#pragma once

#include <cstddef>
#include <vector>

namespace Horo::Editor {
    struct EditorGuiContext;
    class AssetBrowserInteractionSession;
    struct ContentBrowserEntry;
    struct EditorWorkspaceViewCommandData;
    struct EditorWorkspaceViewModel;

    /** @brief Draws actions for the Asset Browser background. */
    void DrawAssetBrowserBackgroundActions(const EditorWorkspaceViewModel &viewModel, AssetBrowserInteractionSession &interactionSession,
                                           EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);

    /** @brief Draws actions for one Asset Browser entry. */
    void DrawAssetBrowserEntryActions(const ContentBrowserEntry &entry, const EditorWorkspaceViewModel &viewModel,
                                      AssetBrowserInteractionSession &interactionSession, EditorWorkspaceViewCommandData &command,
                                      const EditorGuiContext &context);

    /** @brief Reduces Asset Browser keyboard shortcuts to semantic commands. */
    void HandleAssetBrowserShortcuts(const std::vector<std::size_t> &visibleEntries, const EditorWorkspaceViewModel &viewModel,
                                     AssetBrowserInteractionSession &interactionSession, EditorWorkspaceViewCommandData &command);
}  // namespace Horo::Editor
