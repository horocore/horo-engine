/**
 * @file AssetImportModal.cpp
 * @brief ImGui Draw implementation for the Asset Import modal. Compiles only with GUI.
 */

#include "Horo/Editor/AssetImportModal.h"

#include "editor/modals/asset_import/AssetImportModalPresentation.h"

namespace Horo::Editor {
    ModalFrameResult AssetImportModal::Draw() {
        RefreshImportHistory();
        return DrawAssetImportModalPresentation(*this, m_fonts);
    }
}  // namespace Horo::Editor
