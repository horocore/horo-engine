#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::RefreshPhysicsInspectorPort */
    void EditorWorkspaceController::RefreshPhysicsInspectorPort() {
        if (std::this_thread::get_id() != m_physicsOwnerThread)
            return;
        m_viewModel.physicsContext = {};
        if (!m_physicsDocumentOwner.IsValid())
            return;
        const auto epoch = m_document.ReplacementEpoch();
        m_viewModel.physicsContext = {{m_physicsDocumentOwner, epoch}, m_document.Revision().value, m_document.State().value, false, {}};
        if (m_physicsTransactions && m_physicsReplacementEpoch == epoch) {
            m_viewModel.physicsContext.canEdit = true;
            return;
        }
        if (m_physicsTransactions)
            m_physicsTransactions->Shutdown();
        m_physicsTransactions.reset();
        m_physicsReplacementEpoch = epoch;
        auto port = SceneDocumentTransactions::Bind(m_document, m_history, {m_physicsDocumentOwner, epoch});
        if (port.HasError()) {
            m_viewModel.physicsEditError = port.ErrorValue().message;
            return;
        }
        m_physicsTransactions.emplace(std::move(port).Value());
        m_viewModel.physicsContext.canEdit = true;
        m_viewModel.physicsEditError.clear();
    }

    /** @copydoc EditorWorkspaceController::CapturePhysicsInspectorSnapshot */
    Result<SceneTransactionSnapshot> EditorWorkspaceController::CapturePhysicsInspectorSnapshot(
        const EditorWorkspaceViewCommandData &command) const {
        using Return = Result<SceneTransactionSnapshot>;
        const auto &selection = m_selection.Current();
        const auto &request = *command.physicsPayload;
        if (selection.objects.size() != 1 || selection.primary != command.objectPayload ||
            selection.objects.front() != *command.objectPayload || selection.revision.value != request.selectionRevision)
            return Return::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Unauthorized)));
        auto snapshot = m_physicsTransactions->Snapshot();
        if (snapshot.HasError())
            return snapshot;
        if (request.context.session != snapshot.Value().session || request.context.revision != snapshot.Value().revision ||
            request.context.state != snapshot.Value().state)
            return Return::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Stale)));
        return snapshot;
    }

    /** @copydoc EditorWorkspaceController::ApplyPhysicsInspectorCommand */
    void EditorWorkspaceController::ApplyPhysicsInspectorCommand(const EditorWorkspaceViewCommandData &command) {
        const auto reject = [this](const Error &error) {
            m_viewModel.physicsEditError = error.message;
        };
        if (std::this_thread::get_id() != m_physicsOwnerThread)
            return;
        if (!command.objectPayload || !command.physicsPayload)
            return;
        RefreshPhysicsInspectorPort();
        if (!m_physicsTransactions) {
            reject(MakeError(SceneTransactionError(SceneTransactionFailure::Closed)));
            return;
        }
        const auto snapshot = CapturePhysicsInspectorSnapshot(command);
        if (snapshot.HasError()) {
            reject(snapshot.ErrorValue());
            return;
        }
        const auto &request = *command.physicsPayload;
        auto replacement = PreparePhysicsInspectorEdit(snapshot.Value(), *command.objectPayload, request.values);
        if (replacement.HasError()) {
            reject(replacement.ErrorValue());
            return;
        }
        SceneCommandResult event;
        event.object = *command.objectPayload;
        event.kind = DocumentChangeKind::ComponentChanged;
        event.affectedObjects.push_back(event.object);  // Reserve event storage before publication.
        const SceneTransactionContext context{snapshot.Value().session, snapshot.Value().revision, snapshot.Value().state, true,
                                              request.context.cancellation};
        const std::array changes{std::move(replacement).Value()};
        const auto committed = m_physicsTransactions->Execute(context, changes);
        if (committed.HasError()) {
            reject(committed.ErrorValue());
            return;
        }
        event.revision = DocumentRevision{committed.Value().revision};
        event.state = DocumentStateId{committed.Value().state};
        event.committed = committed.Value().committed;
        m_viewModel.physicsEditError.clear();
        HandleDocumentCommandResult(Result<SceneCommandResult>::Success(std::move(event)), "Update Physics components");
    }
}  // namespace Horo::Editor
