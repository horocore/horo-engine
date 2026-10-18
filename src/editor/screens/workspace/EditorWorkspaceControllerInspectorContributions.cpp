#include "Horo/Extensions/EditorInspectorHost.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Reports a zero-publication owner fence rejection through the existing canonical transaction domain. */
        template <class T> Result<T> Reject(SceneTransactionFailure failure = SceneTransactionFailure::Stale) {
            return Result<T>::Failure(MakeError(SceneTransactionError(failure)));
        }

        /** @brief Converts only the explicitly supported scalar presentation profile, without coercion or opaque payload parsing. */
        Result<Extensions::EditorInspectorScalar> Scalar(const Gameplay::BehaviorFieldValue &value) {
            return std::visit([]<class T>(const T &typed) -> Result<Extensions::EditorInspectorScalar> {
                if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, double> ||
                              std::is_same_v<T, std::string>)
                    return Result<Extensions::EditorInspectorScalar>::Success(typed);
                else
                    return Reject<Extensions::EditorInspectorScalar>(SceneTransactionFailure::Invalid);
            }, value);
        }

        /** @brief Matches the complete current ordered selection; providers cannot expand edits to unseen objects or attachments. */
        bool SameSelection(const InspectorExtensionEdit &request, const SelectionSnapshot &selection) {
            if (request.selectionRevision != selection.revision.value || request.targets.size() != selection.objects.size() ||
                request.targets.empty() || request.targets.size() > MaximumInspectorBehaviorSelection)
                return false;
            for (std::size_t i = 0; i < request.targets.size(); ++i)
                if (request.targets[i].object != selection.objects[i])
                    return false;
            return true;
        }

        /** @brief Rechecks the actual project owner's approval, immutable declaration and field constraints for one user gesture. */
        Result<Extensions::ExtensionCapabilityUseLease> AcquireEdit(const InspectorExtensionEdit &request,
                                                                    const Extensions::EditorInspectorContribution &contribution,
                                                                    const std::vector<InspectorProviderApproval> &approvals) {
            if (const auto approval = std::ranges::find_if(approvals,
                                                           [&](const auto &binding) {
                return binding.provider == request.provider && binding.surface == request.surface && binding.behavior == request.behavior &&
                       binding.schemaVersion == request.schemaVersion && binding.documentGeneration == request.documentGeneration &&
                       binding.behaviorRegistryRevision == request.behaviorRegistryRevision &&
                       binding.declaration == contribution.definition &&
                       std::ranges::find(binding.fields, request.field) != binding.fields.end();
            });
                approval == approvals.end())
                return Reject<Extensions::ExtensionCapabilityUseLease>(SceneTransactionFailure::Unauthorized);
            const auto field = std::ranges::find(contribution.definition->fields, request.field, &Extensions::EditorInspectorField::field);
            if (field == contribution.definition->fields.end() ||
                !Extensions::CanEditEditorInspectorField(*contribution.definition, *field))
                return Reject<Extensions::ExtensionCapabilityUseLease>(SceneTransactionFailure::Invalid);
            auto value = Scalar(request.value);
            if (value.HasError())
                return Result<Extensions::ExtensionCapabilityUseLease>::Failure(value.ErrorValue());
            if (auto valid = Extensions::ValidateEditorInspectorValue(*field, value.Value()); valid.HasError())
                return Result<Extensions::ExtensionCapabilityUseLease>::Failure(valid.ErrorValue());
            const auto &capability = contribution.context.Surface().kind == Extensions::EditorSurfaceKind::Inspector
                                         ? Extensions::EditorInspectorCapability
                                         : Extensions::EditorPropertyDrawerCapability;
            return contribution.context.AcquireCapabilityUse(capability);
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::InvalidateInspectorBehaviorBindings */
    void EditorWorkspaceController::InvalidateInspectorBehaviorBindings() {
        m_inspectorApprovals.clear();
        m_viewModel.inspectorApprovals.clear();
        if (m_inspectorBehaviorRegistryRevision == std::numeric_limits<std::uint64_t>::max()) {
            m_inspectorAdmissionClosed = true;
            m_viewModel.inspectorDocumentGeneration = 0;
            return;
        }
        m_viewModel.inspectorBehaviorRegistryRevision = ++m_inspectorBehaviorRegistryRevision;
    }

    /** @copydoc EditorWorkspaceController::RefreshInspectorDocumentPort */
    void EditorWorkspaceController::RefreshInspectorDocumentPort() {
        if (m_inspectorAdmissionClosed) {
            m_viewModel.inspectorDocumentGeneration = 0;
            return;
        }
        const auto current = m_inspectorDocument.CheckCurrent();
        if (current.HasError() &&
            current.ErrorValue().code.Value() == SceneTransactionError(SceneTransactionFailure::WrongThread).code.Value())
            return;  // An invalid caller cannot retire/rebind this port onto a different owner lane.
        if (current.HasValue()) {
            m_viewModel.inspectorDocumentGeneration = m_inspectorDocumentGeneration;
            return;
        }
        m_inspectorDocument.Shutdown();
        m_inspectorApprovals.clear();
        m_viewModel.inspectorApprovals.clear();
        m_viewModel.inspectorDocumentGeneration = 0;
        if (!m_inspectorHost || !m_inspectorDocumentOwner.IsValid() || m_inspectorAdmissionClosed)
            return;
        if (m_inspectorDocumentGeneration == std::numeric_limits<std::uint64_t>::max()) {
            m_inspectorAdmissionClosed = true;
            return;
        }
        auto bound = SceneDocumentTransactions::Bind(m_document, m_history, {m_inspectorDocumentOwner, ++m_inspectorDocumentGeneration});
        if (bound.HasError())
            return;
        m_inspectorDocument = std::move(bound).Value();
        m_viewModel.inspectorDocumentGeneration = m_inspectorDocumentGeneration;
    }

    /** @copydoc EditorWorkspaceController::ApplyInspectorBehaviorEdit */
    Result<SceneTransactionResult> EditorWorkspaceController::ApplyInspectorBehaviorEdit(const InspectorExtensionEdit &request) {
        if (!m_inspectorHost || !m_gameplayRegistry || !request.documentGeneration ||
            request.documentGeneration != m_inspectorDocumentGeneration || request.documentRevision != m_document.Revision().value ||
            request.behaviorRegistryRevision != m_inspectorBehaviorRegistryRevision || m_inspectorAdmissionClosed ||
            !SameSelection(request, m_selection.Current()))
            return Reject<SceneTransactionResult>();
        const auto *behavior = m_gameplayRegistry->Registry().Find(request.behavior);
        if (!behavior || behavior->descriptor.schemaVersion != request.schemaVersion)
            return Reject<SceneTransactionResult>();
        auto contributions = m_inspectorHost->Contributions(request.behavior.Value(), request.schemaVersion);
        if (contributions.HasError())
            return Result<SceneTransactionResult>::Failure(contributions.ErrorValue());
        const auto contribution = std::ranges::find_if(contributions.Value(), [&](const auto &entry) {
            return entry.context.Provider() == request.provider && entry.context.Surface().id == request.surface;
        });
        if (contribution == contributions.Value().end())
            return Reject<SceneTransactionResult>();
        auto lease = AcquireEdit(request, *contribution, m_inspectorApprovals);
        if (lease.HasError())
            return Result<SceneTransactionResult>::Failure(lease.ErrorValue());
        auto snapshot = m_inspectorDocument.Snapshot();
        if (snapshot.HasError())
            return Result<SceneTransactionResult>::Failure(snapshot.ErrorValue());
        auto captured = CaptureInspectorBehavior(snapshot.Value(), request.targets, behavior->descriptor, request.selectionRevision);
        if (captured.HasError())
            return Result<SceneTransactionResult>::Failure(captured.ErrorValue());
        auto changes = PrepareInspectorBehaviorEdit(captured.Value(), request.field, request.value, request.selectionRevision);
        if (changes.HasError())
            return Result<SceneTransactionResult>::Failure(changes.ErrorValue());
        if (!lease.Value().IsUsable() || !contribution->context.IsUsable() || !SameSelection(request, m_selection.Current()))
            return Reject<SceneTransactionResult>();
        const auto &current = snapshot.Value();
        return m_inspectorDocument.Execute({current.session, current.revision, current.state, true, {}}, changes.Value());
    }

    /** @copydoc EditorWorkspaceController::ProcessInspectorContributionCommand */
    bool EditorWorkspaceController::ProcessInspectorContributionCommand(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::ApproveInspectorContribution &&
            command.command != EditorWorkspaceViewCommand::UpdateInspectorBehaviorField)
            return false;
        if (!command.inspectorEdit)
            return true;
        if (command.command == EditorWorkspaceViewCommand::ApproveInspectorContribution) {
            if (auto approved = ApproveInspectorContribution(*command.inspectorEdit); approved.HasError())
                LOG_ERROR("editor.inspector", "Inspector provider approval rejected: %s", approved.ErrorValue().message.c_str());
            return true;
        }
        const auto &request = *command.inspectorEdit;
        if (request.targets.empty() || request.targets.size() > MaximumInspectorBehaviorSelection)
            return true;
        // Prepare the existing host notification payload before the canonical transaction can publish.
        SceneCommandResult notice{.object = request.targets.front().object, .kind = DocumentChangeKind::ComponentChanged};
        notice.affectedObjects.reserve(request.targets.size());
        for (const auto &target : request.targets)
            notice.affectedObjects.push_back(target.object);
        const auto edited = ApplyInspectorBehaviorEdit(request);
        if (edited.HasError()) {
            HandleDocumentCommandResult(Result<SceneCommandResult>::Failure(edited.ErrorValue()), "Edit extension inspector field");
            return true;
        }
        notice.revision = {edited.Value().revision};
        notice.state = {edited.Value().state};
        notice.committed = edited.Value().committed;
        HandleDocumentCommandResult(Result<SceneCommandResult>::Success(std::move(notice)), "Edit extension inspector field");
        return true;
    }

}  // namespace Horo::Editor
