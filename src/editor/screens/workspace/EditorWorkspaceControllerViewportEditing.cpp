#include "EditorInspectionCommandEndpoint.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/project_model/EditorModelErrors.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        /** @brief Owner review uses the same bounded object scope as viewport inspection. */
        constexpr auto MaximumProposalObjects = ObjectInspectionRequest::MaximumObjects;

        /** @brief Reuse the authored Scene rejection vocabulary for viewport admission. */
        Error RejectReview(SceneTransactionFailure failure) {
            return MakeError(SceneTransactionError(failure));
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::CanReviewAgentSceneTransforms */
    bool EditorWorkspaceController::CanReviewAgentSceneTransforms() const {
        const auto active = m_runtimeScene.ActiveScene();
        return InspectSceneRevision().HasValue() && active && HasCurrentInspectionGeometry(*active) && !m_cameraPreview &&
               !m_activeTerrainDocument && !m_viewModel.recoveryAvailable && !m_playSession.IsActive() && !m_assetPlacementPreviewActive &&
               !m_prefabDrop;
    }

    /** @copydoc EditorWorkspaceController::PreviewAgentSceneTransforms */
    Result<std::uint64_t> EditorWorkspaceController::PreviewAgentSceneTransforms(const InspectionSceneRevision revision,
                                                                                 std::span<const SceneObjectTransformUpdate> updates,
                                                                                 CancellationToken cancellation,
                                                                                 std::function<bool()> authorized) {
        using Output = Result<std::uint64_t>;
        ReconcileAgentSceneTransformReview();
        if (updates.empty() || updates.size() > MaximumProposalObjects)
            return Output::Failure(RejectReview(SceneTransactionFailure::Invalid));
        if (authorized && !authorized())
            return Output::Failure(RejectReview(SceneTransactionFailure::Unauthorized));
        const auto current = InspectSceneRevision();
        if (current.HasError() || current.Value() != revision || !CanReviewAgentSceneTransforms())
            return Output::Failure(RejectReview(SceneTransactionFailure::Stale));
        if (m_agentSceneTransformReview || !m_viewport.Current().transformPreviews.empty() || m_viewport.Current().lightPreview ||
            m_agentSceneTransformReviewSerial == std::numeric_limits<std::uint64_t>::max() ||
            revision.epoch == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(RejectReview(SceneTransactionFailure::Unauthorized));
        CancellationSource lifetime{cancellation};
        const SceneTransactionContext context{{m_workspaceSession, revision.epoch + 1},
                                              revision.revision,
                                              m_document.State().value,
                                              true,
                                              lifetime.Token()};
        auto owner = SceneDocumentTransactions::Bind(m_document, m_history, context.session);
        if (owner.HasError())
            return Output::Failure(owner.ErrorValue());
        const auto &objects = m_document.Objects();
        std::vector<SceneObjectReplacement> changes;
        changes.reserve(updates.size());
        for (const auto &update : updates) {
            const auto object = std::ranges::find(objects, update.object, &SceneObjectSnapshot::id);
            const auto visible = std::ranges::find(m_viewModel.objects, update.object, &SceneObject::id);
            if (object == objects.end() || object->generated || visible == m_viewModel.objects.end() || !visible->effectivelyVisible)
                return Output::Failure(RejectReview(SceneTransactionFailure::Invalid));
            const auto fingerprint = FingerprintSceneObjectContent(*object, MaximumSceneObjectFingerprintBytes);
            if (fingerprint.HasError())
                return Output::Failure(fingerprint.ErrorValue());
            auto desired = *object;
            desired.localTransform = update.localTransform;
            changes.push_back({.object = object->id, .priorContent = fingerprint.Value(), .desired = std::move(desired)});
        }
        auto preview = owner.Value().PreviewReplacements(context, std::move(changes), {.maximumChanges = MaximumProposalObjects});
        if (preview.HasError())
            return Output::Failure(preview.ErrorValue());
        std::vector<SceneObjectTransformUpdate> effective;
        for (const auto &change : preview.Value().Changes())
            effective.push_back({change.object, change.desired->localTransform});
        if (effective.empty())
            return Output::Failure(RejectReview(SceneTransactionFailure::Invalid));
        PreviewObjectTransforms(effective);
        if (m_viewport.Current().transformPreviews.size() != effective.size())
            return Output::Failure(RejectReview(SceneTransactionFailure::Invalid));
        const auto id = ++m_agentSceneTransformReviewSerial;
        m_agentSceneTransformReview.emplace(id, std::move(lifetime), context, std::move(owner).Value(), std::move(preview).Value(),
                                            std::move(authorized));
        m_viewModel.agentSceneReview = {id, effective.size()};
        return Output::Success(id);
    }

    /** @copydoc EditorWorkspaceController::ApproveAgentSceneTransforms */
    Result<void> EditorWorkspaceController::ApproveAgentSceneTransforms(const std::uint64_t review) {
        ReconcileAgentSceneTransformReview();
        if (!m_agentSceneTransformReview || m_agentSceneTransformReview->id != review)
            return Result<void>::Failure(RejectReview(SceneTransactionFailure::Stale));
        auto proposal = std::move(*m_agentSceneTransformReview);
        CancelObjectTransformPreview();
        std::vector<SceneObjectId> affected;
        for (const auto &change : proposal.preview.Changes())
            affected.push_back(change.object);
        const SceneTransactionContext approval{proposal.context.session,
                                               m_document.Revision().value,
                                               m_document.State().value,
                                               CanReviewAgentSceneTransforms() && (!proposal.authorized || proposal.authorized()),
                                               {}};
        const auto result = proposal.owner.ApplyReviewedReplacements(approval, std::move(proposal.preview));
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        HandleDocumentCommandResult(Result<SceneCommandResult>::Success({{},
                                                                         {result.Value().revision},
                                                                         {result.Value().state},
                                                                         DocumentChangeKind::TransformChanged,
                                                                         std::move(affected),
                                                                         result.Value().committed}),
                                    "Apply reviewed scene transforms");
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::CancelAgentSceneTransforms */
    bool EditorWorkspaceController::CancelAgentSceneTransforms(const std::uint64_t review) {
        if (!m_agentSceneTransformReview || m_agentSceneTransformReview->id != review)
            return false;
        CancelObjectTransformPreview();
        return true;
    }

    /** @copydoc EditorWorkspaceController::ReconcileAgentSceneTransformReview */
    void EditorWorkspaceController::ReconcileAgentSceneTransformReview() {
        if (!m_agentSceneTransformReview)
            return;
        const auto &review = *m_agentSceneTransformReview;
        if ((review.authorized && !review.authorized()) || review.context.cancellation.IsCancellationRequested() ||
            review.owner.CheckCurrent().HasError() || review.context.revision != m_document.Revision().value ||
            !CanReviewAgentSceneTransforms())
            CancelObjectTransformPreview();
    }

    void EditorWorkspaceController::HandleDocumentCommandResult(const Result<SceneCommandResult> &result, const char *operation) {
        m_agentSceneTransformReview.reset();
        m_viewModel.agentSceneReview.reset();
        if (result.HasError()) {
            LOG_ERROR("editor.scene_document", "%s failed: %s", operation, result.ErrorValue().message.c_str());
            if (result.ErrorValue().code.Value() == SceneDocumentErrors::ObjectLocked.code.Value()) {
                m_notifications.Publish("scene", NotificationSeverity::Warning,
                                        Localized("workspace.hierarchy.locked_edit_blocked",
                                                  "Unlock the object or its parent before editing it."),
                                        Localized("workspace.hierarchy.locked_edit_blocked_title", "Object is locked"),
                                        "scene_object_locked");
            }
            CancelObjectTransformPreview();
            CancelLightComponentPreview();
            return;
        }
        const SceneCommandResult &committed = result.Value();
        if (!committed.committed) {
            CancelObjectTransformPreview();
            CancelLightComponentPreview();
            return;
        }
        m_viewport.ClearTransformPreview();
        m_viewport.ClearLightPreview();
        m_dataBus.Publish(SceneDocumentChangedEvent{committed.revision, committed.state, committed.kind, m_document.IsDirty(),
                                                    committed.affectedObjects});
        m_selection.Reconcile();
        RefreshSceneProjections();
    }

    void EditorWorkspaceController::PreviewObjectTransform(const SceneObjectId object, const Math::Transform &transform) {
        const SceneObjectTransformUpdate update{object, transform};
        PreviewObjectTransforms(std::span{&update, 1});
    }

    void EditorWorkspaceController::PreviewObjectTransforms(const std::span<const SceneObjectTransformUpdate> updates) {
        if (m_agentSceneTransformReview)
            CancelObjectTransformPreview();
        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || m_viewportScene.runtimeSceneId != active->RuntimeId() || updates.empty())
            return;
        if (std::ranges::any_of(updates, [&](const SceneObjectTransformUpdate &update) {
            const std::optional<ResolvedSceneObjectEditorState> state = ResolveSceneObjectEditorState(m_document.Objects(), update.object);
            return !state.has_value() || state->effectivelyLocked;
        }))
            return;

        std::vector<SceneObjectTransformPreview> previews;
        previews.reserve(updates.size());
        for (const SceneObjectTransformUpdate &update : updates)
            previews.emplace_back(update.object, update.localTransform);

        const std::vector<SceneObjectTransformPreview> previousPreviews = m_viewport.Current().transformPreviews;
        if (Result<void> applied = ApplyEditorViewportTransformPreview(*active, previews, m_viewportScene); applied.HasError()) {
            LOG_ERROR("editor.viewport", "Transform preview failed: %s", applied.ErrorValue().message.c_str());
            return;
        }
        if (Result<void> committed = m_viewport.SetTransformPreviews(previews); committed.HasError()) {
            if (const Result<void> restored = ApplyEditorViewportTransformPreview(*active, previousPreviews, m_viewportScene);
                restored.HasError()) {
                LOG_ERROR("editor.viewport", "Transform preview rollback failed: %s", restored.ErrorValue().message.c_str());
            }
            LOG_ERROR("editor.viewport", "Transform preview state failed: %s", committed.ErrorValue().message.c_str());
            return;
        }
        m_viewModel.primarySelectionPreviewWorldTransform.reset();
        if (m_viewModel.primarySelection.has_value()) {
            const auto instance = std::ranges::find(m_viewportScene.instanceObjects, *m_viewModel.primarySelection);
            if (instance != m_viewportScene.instanceObjects.end()) {
                const auto index = static_cast<std::size_t>(std::distance(m_viewportScene.instanceObjects.begin(), instance));
                m_viewModel.primarySelectionPreviewWorldTransform = m_viewportScene.instances[index].localToWorld;
            }
        }
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::CancelObjectTransformPreview() {
        m_agentSceneTransformReview.reset();
        m_viewModel.agentSceneReview.reset();
        if (m_viewport.Current().transformPreviews.empty()) {
            return;
        }
        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || m_viewportScene.runtimeSceneId != active->RuntimeId())
            return;
        if (const Result<void> restored =
                ApplyEditorViewportTransformPreview(*active, std::span<const SceneObjectTransformPreview>{}, m_viewportScene);
            restored.HasError()) {
            LOG_ERROR("editor.viewport", "Transform preview cancellation failed: %s", restored.ErrorValue().message.c_str());
            return;
        }
        m_viewport.ClearTransformPreview();
        m_viewModel.primarySelectionPreviewWorldTransform.reset();
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::PreviewLightComponent(const SceneObjectId object, const Runtime::LightComponent &light) {
        if (const std::optional<ResolvedSceneObjectEditorState> editorState = ResolveSceneObjectEditorState(m_document.Objects(), object);
            !editorState.has_value() || editorState->effectivelyLocked)
            return;
        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || m_viewportScene.runtimeSceneId != active->RuntimeId())
            return;

        const SceneObjectLightPreview preview{object, light};
        const std::optional<SceneObjectLightPreview> previousPreview = m_viewport.Current().lightPreview;
        if (Result<void> applied = ApplyEditorViewportLightPreview(*active, &preview, m_viewportScene); applied.HasError()) {
            LOG_ERROR("editor.viewport", "Light preview failed: %s", applied.ErrorValue().message.c_str());
            return;
        }
        if (Result<void> committed = m_viewport.SetLightPreview(preview); committed.HasError()) {
            const SceneObjectLightPreview *previous = previousPreview.has_value() ? &*previousPreview : nullptr;
            if (const Result<void> restored = ApplyEditorViewportLightPreview(*active, previous, m_viewportScene); restored.HasError())
                LOG_ERROR("editor.viewport", "Light preview rollback failed: %s", restored.ErrorValue().message.c_str());
            LOG_ERROR("editor.viewport", "Light preview state failed: %s", committed.ErrorValue().message.c_str());
        }
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::CancelLightComponentPreview() {
        if (!m_viewport.Current().lightPreview.has_value())
            return;
        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || m_viewportScene.runtimeSceneId != active->RuntimeId())
            return;
        if (const Result<void> restored = ApplyEditorViewportLightPreview(*active, nullptr, m_viewportScene); restored.HasError()) {
            LOG_ERROR("editor.viewport", "Light preview cancellation failed: %s", restored.ErrorValue().message.c_str());
            return;
        }
        m_viewport.ClearLightPreview();
        RefreshViewportLightProjection();
    }

}  // namespace Horo::Editor
