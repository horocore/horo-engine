#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::RefreshWorldStreamingPreview */
    void EditorWorkspaceController::RefreshWorldStreamingPreview() {
        const auto &view = m_viewModel.worldViewport;
        if (!view.snapshot) {
            m_viewModel.worldStreamingPreview.reset();
            m_viewModel.worldStreamingPreviewError.reset();
            if (m_streamingPreview)
                m_streamingPreview->ClearPresentation();
            return;
        }
        if (!m_streamingPreview) {
            m_viewModel.worldStreamingPreview.reset();
            m_viewModel.worldStreamingPreviewError = MakeError(WorldDocumentErrors::Lifecycle);
            return;
        }
        const auto captured = m_streamingPreview->Refresh(view);
        m_viewModel.worldStreamingPreview = captured.HasValue() ? captured.Value() : nullptr;
        m_viewModel.worldStreamingPreviewError = captured.HasError() ? std::optional<Error>{captured.ErrorValue()} : std::nullopt;
    }
}  // namespace Horo::Editor
