#include "Horo/Foundation/Platform.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::ProcessSourceDocumentCommand */
    bool EditorWorkspaceController::ProcessSourceDocumentCommand(const EditorWorkspaceViewCommandData &command) {
        NativeDurableFileSystem files;
        switch (command.command) {
            case EditorWorkspaceViewCommand::SaveSourceDocument:
                if (command.sourceSave)
                    m_sourceSaveOutcome = m_sourceOpenService.Documents().Save(*command.sourceSave, files);
                else
                    m_sourceSaveOutcome = Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::Invalid));
                return true;
            case EditorWorkspaceViewCommand::SaveSourceDocumentAs:
                if (command.sourceSave && command.stringPayload)
                    m_sourceSaveOutcome =
                        m_sourceOpenService.SaveAs(*command.sourceSave, std::filesystem::path{*command.stringPayload}, files);
                else
                    m_sourceSaveOutcome = Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::Invalid));
                return true;
            case EditorWorkspaceViewCommand::SaveAllSourceDocuments:
                m_sourceSaveAllOutcome = m_sourceOpenService.Documents().SaveAll(files);
                return true;
            case EditorWorkspaceViewCommand::CloseSourceDocument:
                if (command.sourceSave)
                    m_sourceCloseOutcome = m_sourceOpenService.ResolveClose(*command.sourceSave, command.sourceCloseDecision, files);
                else
                    m_sourceCloseOutcome = Result<bool>::Failure(MakeError(SourceDocumentErrors::Invalid));
                return true;
            default:
                return false;
        }
    }
}  // namespace Horo::Editor
