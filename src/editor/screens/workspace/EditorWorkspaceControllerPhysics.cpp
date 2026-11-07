#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::RefreshPhysicsProfiles */
    void EditorWorkspaceController::RefreshPhysicsProfiles() {
        m_viewModel.physicsProfiles.clear();
        const auto metadata = Application::LoadProjectMetadata(m_viewModel.projectRoot);
        if (metadata.HasError())
            return;
        const auto document = Application::LoadProjectCollisionSettings(m_viewModel.projectRoot, metadata.Value().projectId);
        if (document.HasError())
            return;
        for (const auto &profile : document.Value().settings->Input().profiles)
            m_viewModel.physicsProfiles.push_back({profile.id, profile.name});
    }

    /** @copydoc EditorWorkspaceController::RefreshPhysicsMaterials */
    void EditorWorkspaceController::RefreshPhysicsMaterials() {
        m_viewModel.physicsMaterials.clear();
        for (const auto &record : m_assetRegistry.Records())
            if (record.type.Value() == "core.physics_material")
                m_viewModel.physicsMaterials.push_back({record.id, std::filesystem::path{record.sourcePath.String()}.stem().string()});
    }

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

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::ApplyColliderViewportCommand */
    void EditorWorkspaceController::ApplyColliderViewportCommand(const EditorWorkspaceViewCommandData &command) {
        if (std::this_thread::get_id() != m_physicsOwnerThread || !command.colliderPayload)
            return;
        RefreshPhysicsInspectorPort();
        const auto &request = *command.colliderPayload;
        const auto reject = [this](const Error &error) {
            m_viewModel.physicsEditError = error.message;
        };
        if (!m_physicsTransactions || m_playSession.IsActive()) {
            reject(MakeError(SceneTransactionError(SceneTransactionFailure::Closed)));
            return;
        }
        const auto snapshot = m_physicsTransactions->Snapshot();
        if (snapshot.HasError()) {
            reject(snapshot.ErrorValue());
            return;
        }
        if (request.context.session != snapshot.Value().session || request.context.revision != snapshot.Value().revision ||
            request.context.state != snapshot.Value().state || request.selectionRevision != m_viewModel.colliderSelectionRevision) {
            reject(MakeError(SceneTransactionError(SceneTransactionFailure::Stale)));
            return;
        }
        if (request.context.cancellation.IsCancellationRequested()) {
            reject(MakeError(SceneTransactionError(SceneTransactionFailure::Cancelled)));
            return;
        }
        if (command.command == EditorWorkspaceViewCommand::SelectPhysicsColliders) {
            if (const auto valid = ValidateColliderViewportSelection(request.selection); valid.HasError()) {
                reject(valid.ErrorValue());
                return;
            }
            m_viewModel.selectedColliders = request.selection;
            ++m_viewModel.colliderSelectionRevision;
            m_viewModel.physicsEditError.clear();
            return;
        }
        CommitColliderViewportEdits(request, snapshot.Value());
    }

    /** @copydoc EditorWorkspaceController::CommitColliderViewportEdits */
    void EditorWorkspaceController::CommitColliderViewportEdits(const ColliderViewportRequest &request,
                                                                const SceneTransactionSnapshot &snapshot) {
        const auto reject = [this](const Error &error) {
            m_viewModel.physicsEditError = error.message;
        };
        if (!request.context.canEdit || request.selection != m_viewModel.selectedColliders ||
            request.edits.size() != request.selection.size()) {
            reject(MakeError(SceneTransactionError(SceneTransactionFailure::Unauthorized)));
            return;
        }
        for (const auto &edit : request.edits)
            if (std::ranges::find(request.selection, edit.target) == request.selection.end()) {
                reject(MakeError(SceneTransactionError(SceneTransactionFailure::Unauthorized)));
                return;
            }
        auto changes = PrepareColliderTransformEdits(snapshot, request.edits);
        if (changes.HasError()) {
            reject(changes.ErrorValue());
            return;
        }
        SceneCommandResult event;
        event.kind = DocumentChangeKind::ComponentChanged;
        for (const auto &change : changes.Value())
            event.affectedObjects.push_back(change.object);
        if (!event.affectedObjects.empty())
            event.object = event.affectedObjects.front();
        const auto committed = m_physicsTransactions->Execute(request.context, changes.Value());
        if (committed.HasError()) {
            reject(committed.ErrorValue());
            return;
        }
        event.revision = {committed.Value().revision};
        event.state = {committed.Value().state};
        event.committed = committed.Value().committed;
        m_viewModel.physicsEditError.clear();
        HandleDocumentCommandResult(Result<SceneCommandResult>::Success(std::move(event)), "Edit collider children");
    }

    /** @copydoc EditorWorkspaceController::ValidateColliderViewportSelection */
    Result<void> EditorWorkspaceController::ValidateColliderViewportSelection(const std::span<const ColliderEditTarget> selection) const {
        if (selection.size() > SceneTransactionLimits{}.maximumChanges) {
            return Result<void>::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Capacity)));
        }
        for (std::size_t index = 0; index < selection.size(); ++index) {
            const auto target = selection[index];
            if (std::ranges::find(selection.begin(), selection.begin() + static_cast<std::ptrdiff_t>(index), target) !=
                selection.begin() + static_cast<std::ptrdiff_t>(index)) {
                return Result<void>::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Invalid)));
            }
            const auto found = std::ranges::find(m_viewModel.viewportColliders, target, &ColliderPresentation::target);
            if (found == m_viewModel.viewportColliders.end() || !found->visible || !found->editable) {
                return Result<void>::Failure(found != m_viewModel.viewportColliders.end() && found->diagnostic
                                                 ? *found->diagnostic
                                                 : MakeError(SceneTransactionError(SceneTransactionFailure::Unauthorized)));
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
