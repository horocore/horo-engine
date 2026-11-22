#include "EditorInspectionCommandEndpoint.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace Horo::Editor {

    namespace {
        const ErrorCodeDescriptor InspectionUnavailable{.domain = ErrorDomainId{"editor.inspection"},
                                                        .code = ErrorCode{"scene_unavailable"},
                                                        .defaultSeverity = ErrorSeverity::Warning,
                                                        .summary = "The authored Scene is unavailable for inspection."};
        const ErrorCodeDescriptor InspectionStale{.domain = ErrorDomainId{"editor.inspection"},
                                                  .code = ErrorCode{"scene_stale"},
                                                  .defaultSeverity = ErrorSeverity::Warning,
                                                  .summary = "The Scene changed since the previous page."};
        const ErrorCodeDescriptor InspectionObjectUnavailable{.domain = ErrorDomainId{"editor.inspection"},
                                                              .code = ErrorCode{"object_unavailable"},
                                                              .defaultSeverity = ErrorSeverity::Warning,
                                                              .summary = "A requested object is missing or hidden."};
        const ErrorCodeDescriptor InspectionInvalidPage{.domain = ErrorDomainId{"editor.inspection"},
                                                        .code = ErrorCode{"invalid_page"},
                                                        .defaultSeverity = ErrorSeverity::Warning,
                                                        .summary = "Provide unique object IDs and a bounded range."};

        /** @brief Projects authored numeric data without names or runtime handles. */
        AgentViewportTransform LocalTransform(const SceneObject &object) {
            const auto &value = object.localTransform;
            return {object.id.value,
                    {value.translation.x, value.translation.y, value.translation.z},
                    {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w},
                    {value.scale.x, value.scale.y, value.scale.z}};
        }

        /** @brief Validates typed page boundaries independently of transport schema admission. */
        bool ValidInspectionPage(const TransformInspectionRequest &request) {
            const auto count = request.objects.size();
            const auto maximum = TransformInspectionRequest::MaximumObjects;
            return count <= maximum && request.offset < count && request.limit > 0 && request.limit <= maximum &&
                   (request.offset == 0 || request.expected.has_value());
        }

        /** @brief Keeps the public numeric context independent of internal editor interaction enums. */
        AgentViewportTool CapturedViewportTool(const EditorTransformTool tool) noexcept {
            switch (tool) {
                case EditorTransformTool::Move:
                    return AgentViewportTool::Move;
                case EditorTransformTool::Rotate:
                    return AgentViewportTool::Rotate;
                case EditorTransformTool::Scale:
                    return AgentViewportTool::Scale;
                case EditorTransformTool::Select:
                    return AgentViewportTool::Select;
            }
            return AgentViewportTool::Select;
        }

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
        if (m_viewModel.projectRoot.empty() || m_viewModel.worldDocument || !m_document.State().IsValid() || !selection.revision.value ||
            selection.objects.empty())
            return std::nullopt;
        if (!m_agentSceneSelectionLifetime)
            m_agentSceneSelectionLifetime.emplace();
        return CaptureAgentSelectionValues(*m_agentSceneSelectionLifetime);
    }

    /** @copydoc EditorWorkspaceController::CaptureAgentViewport */
    std::optional<AgentSceneSelectionSnapshot> EditorWorkspaceController::CaptureAgentViewport(const AgentViewportComponents components) {
        const auto &camera = m_viewport.Current().camera;
        if (m_viewModel.projectRoot.empty() || m_viewModel.worldDocument || !m_document.State().IsValid() ||
            m_viewModel.playState != EditorPlayState::Idle || m_cameraPreview || m_activeTerrainDocument || !camera.IsValid())
            return std::nullopt;
        if (!m_agentViewportLifetime) {
            m_agentViewportLifetime.emplace();
            ++m_agentViewportRevision;
        }
        auto snapshot = CaptureAgentSelectionValues(*m_agentViewportLifetime);
        const bool orthographic = camera.projection == Runtime::CameraProjection::Orthographic;
        snapshot.viewport = AgentViewportOverview{.revision = m_agentViewportRevision,
                                                  .position = {camera.position.x, camera.position.y, camera.position.z},
                                                  .target = {camera.target.x, camera.target.y, camera.target.z},
                                                  .up = {camera.up.x, camera.up.y, camera.up.z},
                                                  .verticalExtent = orthographic ? camera.orthographicHeight : camera.verticalFovRadians,
                                                  .nearPlane = camera.nearPlane,
                                                  .farPlane = camera.farPlane,
                                                  .orthographic = orthographic,
                                                  .tool = CapturedViewportTool(m_viewModel.activeTransformTool)};
        if (components == AgentViewportComponents::LocalTransforms) {
            for (const auto id : std::span{snapshot.objects}.first(snapshot.objectCount)) {
                const auto object = std::ranges::find(m_viewModel.objects, SceneObjectId{id}, &SceneObject::id);
                if (object == m_viewModel.objects.end())
                    return std::nullopt;
                snapshot.transforms[snapshot.transformCount++] = LocalTransform(*object);
            }
        }
        return snapshot;
    }

    /** @copydoc EditorWorkspaceController::BindSceneInspection */
    std::weak_ptr<const EditorInspectionCommandEndpoint> EditorWorkspaceController::BindSceneInspection(
        std::string project, const std::uint64_t authorizationRevision) {
        m_inspectionCommandEndpoint = std::make_shared<const EditorInspectionCommandEndpoint>(
            EditorInspectionCommandEndpoint{std::move(project), authorizationRevision, std::this_thread::get_id(),
                                            [this](const TransformInspectionRequest &request) {
            return InspectLocalTransforms(request);
        }});
        return m_inspectionCommandEndpoint;
    }

    /** @copydoc EditorWorkspaceController::InspectLocalTransforms */
    Result<TransformInspectionPage> EditorWorkspaceController::InspectLocalTransforms(const TransformInspectionRequest &request) const {
        using Output = Result<TransformInspectionPage>;
        if (m_viewModel.projectRoot.empty() || m_viewModel.worldDocument || !m_document.State().IsValid() ||
            m_viewModel.playState != EditorPlayState::Idle)
            return Output::Failure(MakeError(InspectionUnavailable));
        const InspectionSceneRevision scene{m_document.ReplacementEpoch(), m_document.Revision().value};
        if (m_viewModel.assetDropFence.replacementEpoch != scene.epoch || m_viewModel.documentRevision.value != scene.revision)
            return Output::Failure(MakeError(InspectionUnavailable));
        if (request.expected && *request.expected != scene)
            return Output::Failure(MakeError(InspectionStale));
        if (!ValidInspectionPage(request))
            return Output::Failure(MakeError(InspectionInvalidPage));
        TransformInspectionPage page{.scene = scene};
        const auto end = std::min(request.offset + request.limit, request.objects.size());
        for (std::size_t index = 0; index < request.objects.size(); ++index) {
            const auto id = request.objects[index];
            const auto earlier = std::span{request.objects}.first(index);
            if (!id || std::ranges::find(earlier, id) != earlier.end())
                return Output::Failure(MakeError(InspectionInvalidPage));
            const auto object = std::ranges::find(m_viewModel.objects, SceneObjectId{id}, &SceneObject::id);
            if (object == m_viewModel.objects.end() || !object->effectivelyVisible)
                return Output::Failure(MakeError(InspectionObjectUnavailable));
            if (index >= request.offset && index < end)
                page.records.push_back(LocalTransform(*object));
        }
        if (end < request.objects.size())
            page.nextOffset = end;
        return Output::Success(std::move(page));
    }

    /** @copydoc EditorWorkspaceController::CaptureAgentSelectionValues */
    AgentSceneSelectionSnapshot EditorWorkspaceController::CaptureAgentSelectionValues(const CancellationSource &lifetime) const {
        const auto &selection = m_selection.Current();
        AgentSceneSelectionSnapshot snapshot{.documentEpoch = m_document.ReplacementEpoch(),
                                             .documentRevision = m_document.Revision().value,
                                             .selectionRevision = selection.revision.value,
                                             .objectCount = std::min(selection.objects.size(), AgentSceneSelectionSnapshot::MaximumObjects),
                                             .truncated = selection.objects.size() > AgentSceneSelectionSnapshot::MaximumObjects,
                                             .lifetime = lifetime.Token()};
        for (std::size_t index = 0; index < snapshot.objectCount; ++index)
            snapshot.objects[index] = selection.objects[index].value;
        return snapshot;
    }

    /** @copydoc EditorWorkspaceController::InvalidateAgentSceneSelection */
    void EditorWorkspaceController::InvalidateAgentSceneSelection() noexcept {
        InvalidateAgentViewport();
        if (m_agentSceneSelectionLifetime) {
            m_agentSceneSelectionLifetime->RequestCancellation();
            m_agentSceneSelectionLifetime.reset();
        }
    }

    /** @copydoc EditorWorkspaceController::InvalidateAgentViewport */
    void EditorWorkspaceController::InvalidateAgentViewport() noexcept {
        if (m_agentViewportLifetime) {
            m_agentViewportLifetime->RequestCancellation();
            m_agentViewportLifetime.reset();
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
