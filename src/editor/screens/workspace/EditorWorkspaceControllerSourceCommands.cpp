#include "EditorSourcePresentation.h"
#include "EditorWorkspaceController.h"

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::BindSourcePresentation */
    std::weak_ptr<const EditorSourceCommandEndpoint> EditorWorkspaceController::BindSourcePresentation(
        std::string project, const std::uint64_t authorizationRevision) {
        auto endpoint = std::make_shared<const EditorSourceCommandEndpoint>(
            EditorSourceCommandEndpoint{std::move(project), authorizationRevision, std::this_thread::get_id(),
                                        [this](const SourceDocumentId &path, std::optional<EditorSourceCommandPosition> position,
                                               const std::function<bool()> &stop) {
            return PresentSourceCommand(path, SourceCommandLocation(path, position), stop);
        }});
        m_sourceCommandEndpoint = std::move(endpoint);
        return m_sourceCommandEndpoint;
    }

    /** @copydoc EditorWorkspaceController::RevokeSourcePresentation */
    void EditorWorkspaceController::RevokeSourcePresentation() noexcept {
        m_sourceCommandEndpoint.reset();
    }

    /** @copydoc EditorWorkspaceController::PresentSourceCommand */
    Result<void> EditorWorkspaceController::PresentSourceCommand(const SourceDocumentId &path,
                                                                 std::optional<Language::LanguageLocation> target,
                                                                 const std::function<bool()> &stop) {
        auto shown =
            PresentEditorSource(m_sourceOpenService, m_viewModel.workspacePanelHost, path, std::move(target), m_sourceOpenNavigator, stop);
        if (shown.HasError())
            return Result<void>::Failure(shown.ErrorValue());
        m_viewModel.sourceLanguageFocus = std::move(shown).Value();
        RefreshSourceDocumentProjection();
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
