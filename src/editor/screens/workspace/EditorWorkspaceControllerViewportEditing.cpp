#include "EditorInspectionCommandEndpoint.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Utf8.h"
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

    /** @copydoc EditorWorkspaceController::CanReviewAgentSceneEdits */
    bool EditorWorkspaceController::CanReviewAgentSceneEdits() const {
        const auto active = m_runtimeScene.ActiveScene();
        return InspectSceneRevision().HasValue() && active && HasCurrentInspectionGeometry(*active) && !m_cameraPreview &&
               !m_activeTerrainDocument && !m_viewModel.recoveryAvailable && !m_playSession.IsActive() && !m_assetPlacementPreviewActive &&
               !m_prefabDrop;
    }

    /** @copydoc EditorWorkspaceController::CaptureAgentSceneReplacement */
    Result<SceneObjectReplacement> EditorWorkspaceController::CaptureAgentSceneReplacement(const SceneObjectId id) const {
        using Output = Result<SceneObjectReplacement>;
        const auto &objects = m_document.Objects();
        const auto object = std::ranges::find(objects, id, &SceneObjectSnapshot::id);
        const auto visible = std::ranges::find(m_viewModel.objects, id, &SceneObject::id);
        if (object == objects.end() || object->generated || visible == m_viewModel.objects.end() || !visible->effectivelyVisible)
            return Output::Failure(RejectReview(SceneTransactionFailure::Invalid));
        const auto fingerprint = FingerprintSceneObjectContent(*object, MaximumSceneObjectFingerprintBytes);
        if (fingerprint.HasError())
            return Output::Failure(fingerprint.ErrorValue());
        return Output::Success({.object = id, .priorContent = fingerprint.Value(), .desired = *object});
    }

    /** @copydoc EditorWorkspaceController::FindOrCaptureAgentSceneReplacement */
    Result<SceneObjectReplacement *> EditorWorkspaceController::FindOrCaptureAgentSceneReplacement(
        const SceneObjectId id, std::vector<SceneObjectReplacement> &changes) const {
        using Output = Result<SceneObjectReplacement *>;
        const auto found = std::ranges::find(changes, id, &SceneObjectReplacement::object);
        if (found != changes.end())
            return Output::Success(std::to_address(found));
        auto captured = CaptureAgentSceneReplacement(id);
        if (captured.HasError())
            return Output::Failure(captured.ErrorValue());
        return Output::Success(&changes.emplace_back(std::move(captured).Value()));
    }

    /** @copydoc EditorWorkspaceController::StageAgentSceneLights */
    Result<void> EditorWorkspaceController::StageAgentSceneLights(const std::span<const AgentSceneLightUpdate> lights,
                                                                  std::vector<SceneObjectReplacement> &changes) const {
        for (const auto &update : lights) {
            if (std::ranges::count(lights, update.object, &AgentSceneLightUpdate::object) != 1)
                return Result<void>::Failure(RejectReview(SceneTransactionFailure::Invalid));
            const auto change = FindOrCaptureAgentSceneReplacement({update.object}, changes);
            if (change.HasError())
                return Result<void>::Failure(change.ErrorValue());
            auto &light = change.Value()->desired->components.light;
            if (!light)
                return Result<void>::Failure(RejectReview(SceneTransactionFailure::Invalid));
            light->enabled = update.enabled;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::PreviewAgentSceneEdits */
    Result<std::uint64_t> EditorWorkspaceController::PreviewAgentSceneEdits(const InspectionSceneRevision revision,
                                                                            std::span<const SceneObjectTransformUpdate> updates,
                                                                            std::span<const AgentSceneObjectRename> renames,
                                                                            CancellationToken cancellation,
                                                                            std::function<bool()> authorized,
                                                                            std::span<const AgentSceneLightUpdate> lights) {
        using Output = Result<std::uint64_t>;
        ReconcileAgentSceneReview();
        if (updates.size() > MaximumProposalObjects || renames.size() > MaximumProposalObjects - updates.size() ||
            lights.size() > MaximumProposalObjects - updates.size() - renames.size() ||
            (updates.empty() && renames.empty() && lights.empty()))
            return Output::Failure(RejectReview(SceneTransactionFailure::Invalid));
        if (authorized && !authorized())
            return Output::Failure(RejectReview(SceneTransactionFailure::Unauthorized));
        const auto current = InspectSceneRevision();
        if (current.HasError() || current.Value() != revision || !CanReviewAgentSceneEdits())
            return Output::Failure(RejectReview(SceneTransactionFailure::Stale));
        if (m_agentSceneReview || !m_viewport.Current().transformPreviews.empty() || m_viewport.Current().lightPreview ||
            m_agentSceneReviewSerial == std::numeric_limits<std::uint64_t>::max() ||
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
        std::vector<SceneObjectReplacement> changes;
        changes.reserve(updates.size() + renames.size() + lights.size());
        for (const auto &update : updates) {
            auto replacement = CaptureAgentSceneReplacement(update.object);
            if (replacement.HasError())
                return Output::Failure(replacement.ErrorValue());
            auto change = std::move(replacement).Value();
            change.desired->localTransform = update.localTransform;
            changes.push_back(std::move(change));
        }
        for (const auto &rename : renames) {
            if (!IsValidSceneObjectName(rename.name) || !IsValidUtf8ScalarSequence(rename.name) ||
                std::ranges::any_of(rename.name,
                                    [](const unsigned char byte) {
                return byte < 0x20 || byte == 0x7f;
            }) ||
                std::ranges::count(renames, rename.object, &AgentSceneObjectRename::object) != 1)
                return Output::Failure(RejectReview(SceneTransactionFailure::Invalid));
            const auto replacement = FindOrCaptureAgentSceneReplacement({rename.object}, changes);
            if (replacement.HasError())
                return Output::Failure(replacement.ErrorValue());
            replacement.Value()->desired->name = rename.name;
        }
        if (const auto staged = StageAgentSceneLights(lights, changes); staged.HasError())
            return Output::Failure(staged.ErrorValue());
        auto preview = owner.Value().PreviewReplacements(context, std::move(changes), {.maximumChanges = MaximumProposalObjects});
        if (preview.HasError())
            return Output::Failure(preview.ErrorValue());
        if (preview.Value().Changes().empty())
            return Output::Failure(RejectReview(SceneTransactionFailure::Invalid));
        EditorWorkspaceViewModel::AgentSceneReviewView view{.objectCount = preview.Value().Changes().size()};
        std::vector<SceneObjectTransformUpdate> effective;
        const auto &objects = m_document.Objects();
        for (const auto &change : preview.Value().Changes()) {
            const auto &prior = *std::ranges::find(objects, change.object, &SceneObjectSnapshot::id);
            if (prior.localTransform != change.desired->localTransform)
                effective.push_back({change.object, change.desired->localTransform});
            if (prior.name != change.desired->name)
                view.renames.push_back({prior.id.value, prior.name, change.desired->name});
            if (prior.components.light != change.desired->components.light)
                view.lights.push_back({prior.id.value, prior.name, change.desired->components.light->enabled});
        }
        if (!effective.empty())
            PreviewObjectTransforms(effective);
        if (m_viewport.Current().transformPreviews.size() != effective.size())
            return Output::Failure(RejectReview(SceneTransactionFailure::Invalid));
        auto kind = view.renames.empty() ? DocumentChangeKind::TransformChanged : DocumentChangeKind::Renamed;
        if (!view.lights.empty())
            kind = DocumentChangeKind::ComponentChanged;
        view.id = ++m_agentSceneReviewSerial;
        m_agentSceneReview.emplace(view.id, std::move(lifetime), context, std::move(owner).Value(), std::move(preview).Value(),
                                   std::move(authorized), kind);
        m_viewModel.agentSceneReview = std::move(view);
        return Output::Success(m_agentSceneReviewSerial);
    }

    /** @copydoc EditorWorkspaceController::ApproveAgentSceneReview */
    Result<void> EditorWorkspaceController::ApproveAgentSceneReview(const std::uint64_t review) {
        ReconcileAgentSceneReview();
        if (!m_agentSceneReview || m_agentSceneReview->id != review)
            return Result<void>::Failure(RejectReview(SceneTransactionFailure::Stale));
        auto proposal = std::move(*m_agentSceneReview);
        CancelObjectTransformPreview();
        std::vector<SceneObjectId> affected;
        for (const auto &change : proposal.preview.Changes())
            affected.push_back(change.object);
        const SceneTransactionContext approval{proposal.context.session,
                                               m_document.Revision().value,
                                               m_document.State().value,
                                               CanReviewAgentSceneEdits() && (!proposal.authorized || proposal.authorized()),
                                               {}};
        const auto result = proposal.owner.ApplyReviewedReplacements(approval, std::move(proposal.preview));
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        HandleDocumentCommandResult(Result<SceneCommandResult>::Success({{},
                                                                         {result.Value().revision},
                                                                         {result.Value().state},
                                                                         proposal.kind,
                                                                         std::move(affected),
                                                                         result.Value().committed}),
                                    "Apply reviewed scene edits");
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::CancelAgentSceneReview */
    bool EditorWorkspaceController::CancelAgentSceneReview(const std::uint64_t review) {
        if (!m_agentSceneReview || m_agentSceneReview->id != review)
            return false;
        CancelObjectTransformPreview();
        return true;
    }

    /** @copydoc EditorWorkspaceController::ReconcileAgentSceneReview */
    void EditorWorkspaceController::ReconcileAgentSceneReview() {
        if (!m_agentSceneReview)
            return;
        const auto &review = *m_agentSceneReview;
        if ((review.authorized && !review.authorized()) || review.context.cancellation.IsCancellationRequested() ||
            review.owner.CheckCurrent().HasError() || review.context.revision != m_document.Revision().value || !CanReviewAgentSceneEdits())
            CancelObjectTransformPreview();
    }

    void EditorWorkspaceController::HandleDocumentCommandResult(const Result<SceneCommandResult> &result, const char *operation) {
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
        if (m_agentSceneReview)
            CancelObjectTransformPreview();
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
        if (m_agentSceneReview)
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
        const bool retiringReview = m_agentSceneReview.has_value();
        m_agentSceneReview.reset();
        m_viewModel.agentSceneReview.reset();
        if (m_viewport.Current().transformPreviews.empty() && (!retiringReview || m_viewportScene.transformOverrides.empty())) {
            return;
        }
        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        bool restored = false;
        if (active && m_viewportScene.runtimeSceneId == active->RuntimeId()) {
            const auto result = ApplyEditorViewportTransformPreview(*active, {}, m_viewportScene);
            restored = result.HasValue();
            if (!restored)
                LOG_ERROR("editor.viewport", "Transform preview cancellation failed: %s", result.ErrorValue().message.c_str());
        }
        if (!restored && !retiringReview)
            return;
        if (!restored) {
            // A retired proposal may never be reapplied to the replacement viewport.
            m_viewportScene = {};
            m_viewportScene.camera = m_viewport.Current().camera;
            m_activeRuntimeRevision = {};
            AdvanceViewportSceneRevision();
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
