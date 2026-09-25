#pragma once

/**
 * @file AssetImportModalPresentationDetails.h
 * @brief Target-private entry point for the Asset Import settings panel.
 */

#include "Horo/Editor/EditorTheme.h"

namespace Horo::Assets {
    struct AssetImportSnapshot;
}

namespace Horo::Editor {
    class AssetImportModal;

    /**
     * @brief Draws the selected import item's preview and settings.
     * @param modal Modal state and actions.
     * @param snapshot Import queue snapshot.
     * @param fonts Editor typography.
     * @param height Available panel height.
     */
    void DrawAssetImportDetails(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Theme::Fonts &fonts,
                                float height);
}  // namespace Horo::Editor
