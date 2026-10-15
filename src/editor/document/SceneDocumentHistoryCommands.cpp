#include "editor/document/SceneDocumentInternal.h"

namespace Horo::Editor {
    using namespace SceneDocumentDetail;

    /** @copydoc SceneDocumentCommandExecutor::Undo */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Undo() {
        if (!m_history.m_impl->undo.empty() && std::holds_alternative<SceneObjectsReplacedDelta>(m_history.m_impl->undo.back().delta))
            return SceneDocumentHistoryReplay::Execute(m_document, m_history, false);
        if (m_history.m_impl->undo.empty()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::NothingToUndo, "No committed scene command is available to undo."));
        }
        if (std::holds_alternative<PrefabInstanceValuesDelta>(m_history.m_impl->undo.back().delta))
            return SceneDocumentPrefabValueCommands{m_document, m_history}.Replay(true);
        HistoryRecord entry = std::move(m_history.m_impl->undo.back());
        m_history.m_impl->undo.pop_back();
        RevertDelta(m_document.m_objects, m_document.m_prefabInstances, entry.delta);
        ++m_document.m_revision.value;
        m_document.m_state = entry.beforeState;
        SceneCommandResult result = HistoryCommandResult(m_document, entry, DocumentChangeKind::Undone);
        m_history.m_impl->redo.push_back(std::move(entry));
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentCommandExecutor::Redo */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Redo() {
        if (!m_history.m_impl->redo.empty() && std::holds_alternative<SceneObjectsReplacedDelta>(m_history.m_impl->redo.back().delta))
            return SceneDocumentHistoryReplay::Execute(m_document, m_history, true);
        if (m_history.m_impl->redo.empty()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::NothingToRedo, "No reverted scene command is available to redo."));
        }
        if (std::holds_alternative<PrefabInstanceValuesDelta>(m_history.m_impl->redo.back().delta))
            return SceneDocumentPrefabValueCommands{m_document, m_history}.Replay(false);
        HistoryRecord entry = std::move(m_history.m_impl->redo.back());
        m_history.m_impl->redo.pop_back();
        ApplyDelta(m_document.m_objects, m_document.m_prefabInstances, entry.delta);
        ++m_document.m_revision.value;
        m_document.m_state = entry.afterState;
        SceneCommandResult result = HistoryCommandResult(m_document, entry, DocumentChangeKind::Redone);
        m_history.m_impl->undo.push_back(std::move(entry));
        return Result<SceneCommandResult>::Success(std::move(result));
    }

}  // namespace Horo::Editor
