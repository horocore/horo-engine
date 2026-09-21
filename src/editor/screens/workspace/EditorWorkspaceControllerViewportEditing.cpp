#include "Horo/Foundation/Logging/Logger.h"
#include "editor/project_model/EditorModelErrors.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Editor {

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
