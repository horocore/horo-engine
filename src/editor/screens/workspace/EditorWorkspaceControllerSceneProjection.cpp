#include "Horo/Foundation/Logging/Logger.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace Horo::Editor {

    namespace {
        /** @brief Classifies the complete typed authored component set for hierarchy presentation. */
        SceneObjectKind PresentedKind(const SceneObjectSnapshot &object) {
            int componentCount = 0;
            using enum SceneObjectKind;
            SceneObjectKind singleComponentKind = GameObject;

            if (object.primitiveMesh.has_value() || object.meshAsset.has_value()) {
                componentCount++;
                singleComponentKind = Mesh;
            }
            if (object.components.camera.has_value()) {
                componentCount++;
                singleComponentKind = Camera;
            }
            if (object.components.light.has_value()) {
                componentCount++;
                singleComponentKind = Light;
            }
            if (object.components.triggerVolume.has_value()) {
                componentCount++;
                singleComponentKind = TriggerVolume;
            }
            if (object.components.audioSource.has_value()) {
                componentCount++;
                singleComponentKind = AudioSource;
            }

            if (object.components.characterController) {
                ++componentCount;
                singleComponentKind = GameObject;
            }

            return componentCount == 1 ? singleComponentKind : GameObject;
        }
    }  // namespace

    void EditorWorkspaceController::RefreshSceneProjections() {
        RefreshInspectorDocumentPort();
        ReconcileAudioPreview();
        const SceneDocumentSnapshot documentSnapshot = m_document.Snapshot();
        m_viewModel.documentRevision = documentSnapshot.revision;
        RefreshPhysicsInspectorPort();
        RefreshNavigationInspectorDefinitions();
        m_viewModel.objects.clear();
        m_viewModel.objects.reserve(documentSnapshot.objects.size());
        for (const SceneObjectSnapshot &object : documentSnapshot.objects) {
            const SceneObjectKind kind = PresentedKind(object);
            const ResolvedSceneObjectEditorState editorState = *ResolveSceneObjectEditorState(documentSnapshot.objects, object.id);
            m_viewModel.objects.push_back(SceneObject{.id = object.id,
                                                      .parent = object.parent,
                                                      .name = object.name,
                                                      .kind = kind,
                                                      .localTransform = object.localTransform,
                                                      .components = object.components,
                                                      .editorState = object.editorState,
                                                      .effectivelyVisible = editorState.effectivelyVisible,
                                                      .effectivelyLocked = editorState.effectivelyLocked,
                                                      .hiddenByParent = editorState.hiddenByParent,
                                                      .lockedByParent = editorState.lockedByParent});
        }
        const auto colliders = ProjectPhysicsColliders(documentSnapshot.objects);
        m_viewModel.viewportColliders.clear();
        if (colliders.HasValue())
            m_viewModel.viewportColliders = colliders.Value();
        else
            m_viewModel.physicsEditError = colliders.ErrorValue().message;
        const auto oldColliderCount = m_viewModel.selectedColliders.size();
        std::erase_if(m_viewModel.selectedColliders, [this](const auto target) {
            return std::ranges::none_of(m_viewModel.viewportColliders, [target](const auto &value) {
                return value.target == target;
            });
        });
        if (oldColliderCount != m_viewModel.selectedColliders.size())
            ++m_viewModel.colliderSelectionRevision;
        RefreshPrefabHierarchy(documentSnapshot);
        m_viewModel.isDirty = m_document.IsDirty();
        m_viewModel.canUndo = m_history.CanUndo();
        m_viewModel.canRedo = m_history.CanRedo();
        QueueRuntimeScene(documentSnapshot);
        RefreshSelectionProjection();
    }

    void EditorWorkspaceController::QueueRuntimeScene(SceneDocumentSnapshot snapshot) {
        if (snapshot.state.value == m_activeRuntimeRevision.value || snapshot.state.value == m_queuedDefinitionRevision.value ||
            (m_deferredRuntimeSnapshot && m_deferredRuntimeSnapshot->state == snapshot.state))
            return;

        Result<Runtime::RuntimeSceneDefinition> definition = ConvertSceneDocumentToRuntime(snapshot, m_previewSceneId, m_triggerBinding);
        if (definition.HasError()) {
            LOG_ERROR("editor.runtime_scene", "Scene conversion failed: %s", definition.ErrorValue().message.c_str());
            return;
        }
        if (const Result<void> queued = m_runtimeScene.QueuePreparation(definition.Value()); queued.HasError()) {
            m_deferredRuntimeSnapshot = std::move(snapshot);
            return;
        }
        m_queuedRuntimeRevision = snapshot.revision;
        m_queuedDefinitionRevision = Runtime::SceneDefinitionRevision{snapshot.state.value};
    }

    void EditorWorkspaceController::SynchronizeRuntimeScenePreview() {
        if (m_playSession.IsActive()) {
            ExtractPlayViewportScene();
            return;
        }
        if (std::optional<Error> operationError = m_runtimeScene.TakeOperationError())
            LOG_ERROR("editor.runtime_scene", "Runtime scene operation failed: %s", operationError->message.c_str());

        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || (!m_cameraPreview && active->DefinitionRevision() == m_activeRuntimeRevision))
            return;

        LoadDocumentAssetMeshes();
        const SceneDocumentSnapshot document = m_document.Snapshot();
        const auto camera = ResolveCameraCutPreview(*active);
        if (camera.HasError()) {
            LOG_ERROR("editor.viewport", "Camera authority commit failed: %s", camera.ErrorValue().message.c_str());
            return;
        }
        Result<EditorViewportSceneSnapshot> extracted = ExtractEditorViewportScene(*active, m_queuedRuntimeRevision, camera.Value(),
                                                                                   m_primitiveMeshCache, &document, &m_assetMeshCache);
        if (extracted.HasError()) {
            LOG_ERROR("editor.viewport", "Runtime scene extraction failed: %s", extracted.ErrorValue().message.c_str());
            return;
        }
        m_viewportScene = std::move(extracted).Value();
        m_viewModel.viewportCamera = camera.Value();
        ++m_viewportSceneRevision;
        if (!m_viewport.Current().transformPreviews.empty()) {
            const Result<void> reapplied =
                ApplyEditorViewportTransformPreview(*active, m_viewport.Current().transformPreviews, m_viewportScene);
            if (reapplied.HasError())
                LOG_ERROR("editor.viewport", "Transform preview reapply failed: %s", reapplied.ErrorValue().message.c_str());
        }
        if (m_viewport.Current().lightPreview.has_value()) {
            const Result<void> reapplied = ApplyEditorViewportLightPreview(*active, &*m_viewport.Current().lightPreview, m_viewportScene);
            if (reapplied.HasError())
                LOG_ERROR("editor.viewport", "Light preview reapply failed: %s", reapplied.ErrorValue().message.c_str());
        }
        RefreshViewportLightProjection();
        m_activeRuntimeRevision = active->DefinitionRevision();
        if (m_queuedDefinitionRevision == m_activeRuntimeRevision)
            m_queuedDefinitionRevision = {};
        m_selection.Reconcile();
        RefreshSelectionProjection();

        if (m_deferredRuntimeSnapshot && m_deferredRuntimeSnapshot->state.value != m_activeRuntimeRevision.value) {
            SceneDocumentSnapshot deferred = std::move(*m_deferredRuntimeSnapshot);
            m_deferredRuntimeSnapshot.reset();
            QueueRuntimeScene(std::move(deferred));
        }
    }

}  // namespace Horo::Editor
