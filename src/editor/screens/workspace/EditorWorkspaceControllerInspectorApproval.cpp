#include "Horo/Extensions/EditorInspectorHost.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Returns existing owner-domain approval rejection without modifying either registry or document. */
        Result<void> Denied(SceneTransactionFailure reason = SceneTransactionFailure::Unauthorized) {
            return Result<void>::Failure(MakeError(SceneTransactionError(reason)));
        }

        /** @brief Proves every approved field belongs to the actual host schema and current exact scalar profile. */
        bool ActualFields(const InspectorBehaviorDraft &actual, const Extensions::EditorInspectorDefinition &declaration) {
            return std::ranges::all_of(declaration.fields, [&](const auto &field) {
                const auto found = std::ranges::find(actual.fields, field.field, &InspectorBehaviorValue::field);
                if (found == actual.fields.end())
                    return false;
                return std::visit([&](const auto &value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, double> ||
                                  std::is_same_v<T, std::string>)
                        return Extensions::ValidateEditorInspectorValue(field, Extensions::EditorInspectorScalar{value}).HasValue();
                    else
                        return false;
                }, found->value);
            });
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::ApproveInspectorContribution */
    Result<void> EditorWorkspaceController::ApproveInspectorContribution(const InspectorExtensionEdit &request) {
        const auto &selection = m_selection.Current();
        if (!m_inspectorHost || !m_gameplayRegistry || !request.documentGeneration ||
            request.documentGeneration != m_inspectorDocumentGeneration || request.documentRevision != m_document.Revision().value ||
            request.behaviorRegistryRevision != m_inspectorBehaviorRegistryRevision || m_inspectorAdmissionClosed ||
            request.selectionRevision != selection.revision.value || request.targets.empty() ||
            request.targets.size() != selection.objects.size())
            return Denied();
        for (std::size_t i = 0; i < request.targets.size(); ++i)
            if (request.targets[i].object != selection.objects[i])
                return Denied();
        const auto *behavior = m_gameplayRegistry->Registry().Find(request.behavior);
        if (!behavior || behavior->descriptor.schemaVersion != request.schemaVersion)
            return Denied();
        auto available = m_inspectorHost->Contributions(request.behavior.Value(), request.schemaVersion);
        if (available.HasError())
            return Result<void>::Failure(available.ErrorValue());
        const auto contribution = std::ranges::find_if(available.Value(), [&](const auto &entry) {
            return entry.context.Provider() == request.provider && entry.context.Surface().id == request.surface;
        });
        if (contribution == available.Value().end())
            return Denied();
        auto current = m_inspectorDocument.Snapshot();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        auto actual = CaptureInspectorBehavior(current.Value(), request.targets, behavior->descriptor, request.selectionRevision);
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        if (!ActualFields(actual.Value(), *contribution->definition))
            return Denied(SceneTransactionFailure::Invalid);
        return PublishInspectorApproval(request, *contribution);
    }

    /** @copydoc EditorWorkspaceController::PublishInspectorApproval */
    Result<void> EditorWorkspaceController::PublishInspectorApproval(const InspectorExtensionEdit &request,
                                                                     const Extensions::EditorInspectorContribution &contribution) {
        const auto duplicate = std::ranges::find_if(m_inspectorApprovals, [&](const auto &approved) {
            return approved.provider == request.provider && approved.declaration == contribution.definition &&
                   approved.documentGeneration == request.documentGeneration &&
                   approved.behaviorRegistryRevision == request.behaviorRegistryRevision;
        });
        if (duplicate != m_inspectorApprovals.end())
            return Result<void>::Success();
        if (m_inspectorApprovalRevision == std::numeric_limits<std::uint64_t>::max()) {
            m_inspectorAdmissionClosed = true;
            return Denied(SceneTransactionFailure::Capacity);
        }
        InspectorProviderApproval approved{request.provider,
                                           request.surface,
                                           request.behavior,
                                           request.schemaVersion,
                                           request.documentGeneration,
                                           request.behaviorRegistryRevision,
                                           {},
                                           contribution.definition};
        for (const auto &field : contribution.definition->fields)
            approved.fields.push_back(field.field);
        auto candidate = m_inspectorApprovals;
        // The explicit action replaces overlapping presentation authority for this exact
        // type/schema. Fields outside the new declaration return to generic controls.
        std::erase_if(candidate, [&](const auto &previous) {
            return previous.behavior == approved.behavior && previous.schemaVersion == approved.schemaVersion &&
                   std::ranges::any_of(previous.fields, [&](const auto &field) {
                return std::ranges::find(approved.fields, field) != approved.fields.end();
            });
        });
        if (candidate.size() >= 128)
            return Denied(SceneTransactionFailure::Capacity);
        candidate.push_back(std::move(approved));
        auto projection = candidate;
        if (!contribution.context.IsUsable())
            return Denied();
        m_inspectorApprovals.swap(candidate);
        m_viewModel.inspectorApprovals.swap(projection);
        m_viewModel.inspectorApprovalRevision = ++m_inspectorApprovalRevision;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
