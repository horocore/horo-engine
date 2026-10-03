/**
 * @file AssetImportModal.cpp
 * @brief ImGui Draw implementation for the Asset Import modal. Compiles only with GUI.
 */

#include "Horo/Editor/AssetImportModal.h"

#include "editor/modals/asset_import/AssetImportModalPresentation.h"
#include "editor/modals/asset_import/AssetImportSourcePreview.h"

namespace Horo::Editor {
    ModalFrameResult AssetImportModal::Draw() {
        RefreshImportHistory();
        if (m_sourcePreview) {
            const std::size_t index = m_snapshot.selectedItemIndex;
            const bool visible = index < m_snapshot.items.size() && IsItemVisible(index);
            m_sourcePreview->Update(visible ? &m_snapshot.items[index] : nullptr, visible ? ImporterFor(index) : nullptr);
        }
        return DrawAssetImportModalPresentation(*this, m_fonts);
    }
}  // namespace Horo::Editor
