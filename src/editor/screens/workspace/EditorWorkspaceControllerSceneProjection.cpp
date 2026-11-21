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

    /** @copydoc EditorWorkspaceController::CaptureAgentSceneSelection */
    std::optional<AgentSceneSelectionSnapshot> EditorWorkspaceController::CaptureAgentSceneSelection() {
        const auto &selection = m_selection.Current();
        const auto documentRevision = m_document.Revision().value;
        if (m_viewModel.projectRoot.empty() || m_viewModel.worldDocument || !m_document.State().IsValid() || !selection.revision.value ||
            selection.objects.empty())
            return std::nullopt;
        if (!m_agentSceneSelectionLifetime)
            m_agentSceneSelectionLifetime.emplace();
        AgentSceneSelectionSnapshot snapshot{.documentEpoch = m_document.ReplacementEpoch(),
                                             .documentRevision = documentRevision,
                                             .selectionRevision = selection.revision.value,
                                             .objectCount = std::min(selection.objects.size(), AgentSceneSelectionSnapshot::MaximumObjects),
                                             .truncated = selection.objects.size() > AgentSceneSelectionSnapshot::MaximumObjects,
                                             .lifetime = m_agentSceneSelectionLifetime->Token()};
        for (std::size_t index = 0; index < snapshot.objectCount; ++index)
            snapshot.objects[index] = selection.objects[index].value;
        return snapshot;
    }

    /** @copydoc EditorWorkspaceController::InvalidateAgentSceneSelection */
    void EditorWorkspaceController::InvalidateAgentSceneSelection() noexcept {
        if (m_agentSceneSelectionLifetime) {
            m_agentSceneSelectionLifetime->RequestCancellation();
            m_agentSceneSelectionLifetime.reset();
        }
    }

    void EditorWorkspaceController::RefreshSceneProjections() {
        RefreshInspectorDocumentPort();
        ReconcileAudioPreview();
        const SceneDocumentSnapshot documentSnapshot = m_document.Snapshot();
        if (m_viewModel.documentRevision != documentSnapshot.revision ||
            m_viewModel.assetDropFence.replacementEpoch != m_document.ReplacementEpoch())
            InvalidateAgentSceneSelection();
        m_viewModel.documentRevision = documentSnapshot.revision;
        m_viewModel.assetDropFence = {m_workspaceSession, documentSnapshot.revision, m_document.ReplacementEpoch()};
        m_viewModel.navigationScenePath =
            m_defaultScenePath ? m_defaultScenePath->lexically_relative(ProjectRoot()) : std::filesystem::path{};
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

        auto definition = BuildRuntimeSceneProjection(snapshot);
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

    Result<Runtime::RuntimeSceneDefinition> EditorWorkspaceController::BuildRuntimeSceneProjection(const SceneDocumentSnapshot &snapshot) {
        Result<Runtime::RuntimeSceneDefinition> definition = ConvertSceneDocumentToRuntime(snapshot, m_previewSceneId, m_triggerBinding);
        if (!snapshot.prefabInstances.empty() && m_prefabSources && m_viewModel.prefabHierarchy &&
            !m_viewModel.prefabHierarchyUnavailable && m_viewModel.prefabHierarchy->documentRevision == snapshot.revision.value) {
            auto limits = PrefabLimits();
            if (limits.HasValue())
                definition = ConvertScenePrefabProjectionToRuntime(snapshot, m_previewSceneId, *m_viewModel.prefabHierarchy->sources,
                                                                   m_prefabSources->resolver, limits.Value(), nullptr, m_triggerBinding);
        }
        return definition;
    }

    /** @copydoc EditorWorkspaceController::SynchronizeRuntimeScenePreview() */
    void EditorWorkspaceController::SynchronizeRuntimeScenePreview() {
        if (const auto synced = SynchronizeRuntimeScenePreview(std::nullopt); synced.HasError())
            LOG_ERROR("editor.viewport", "Camera authority commit failed: %s", synced.ErrorValue().message.c_str());
    }

    /** @copydoc EditorWorkspaceController::SynchronizeRuntimeScenePreview(std::optional<EditorViewportCrossFadeCapacity>) */
    Result<void> EditorWorkspaceController::SynchronizeRuntimeScenePreview(const std::optional<EditorViewportCrossFadeCapacity> capacity) {
        if (m_playSession.IsActive()) {
            ExtractPlayViewportScene();
            return Result<void>::Success();
        }
        if (std::optional<Error> operationError = m_runtimeScene.TakeOperationError())
            LOG_ERROR("editor.runtime_scene", "Runtime scene operation failed: %s", operationError->message.c_str());

        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || (!m_cameraPreview && active->DefinitionRevision() == m_activeRuntimeRevision))
            return Result<void>::Success();

        LoadDocumentAssetMeshes();
        const SceneDocumentSnapshot document = m_document.Snapshot();
        auto camera = ResolveCameraCutPreview(*active, capacity);
        if (camera.HasError())
            return Result<void>::Failure(std::move(camera).ErrorValue());
        Result<EditorViewportSceneSnapshot> extracted =
            ExtractEditorViewportScene(*active, m_queuedRuntimeRevision, camera.Value().destination, m_primitiveMeshCache, &document,
                                       &m_assetMeshCache);
        if (extracted.HasError()) {
            LOG_ERROR("editor.viewport", "Runtime scene extraction failed: %s", extracted.ErrorValue().message.c_str());
            return Result<void>::Success();
        }
        m_viewportScene = std::move(extracted).Value();
        m_viewportScene.cameraCrossFade = camera.Value().crossFade;
        m_viewModel.viewportCamera = camera.Value().destination;
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
        return Result<void>::Success();
    }

}  // namespace Horo::Editor
