#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] std::filesystem::path ResolveAssetSourcePath(const EditorWorkspaceViewModel &viewModel,
                                                                   const AssetSceneDropRequest &request,
                                                                   const Assets::AssetRecord &record) {
            if (const std::filesystem::path draggedPath{request.absoluteAssetPath}; draggedPath.is_absolute())
                return draggedPath;
            return std::filesystem::path{viewModel.projectRoot} / record.sourcePath.String();
        }

        /** @brief Preserves explicit UTF-8 source identity when constructing native paths, including Windows wide paths. */
        std::filesystem::path SourcePathFromUtf8(const std::string_view encoded) {
            return std::filesystem::path{std::u8string{encoded.begin(), encoded.end()}};
        }

        /** @brief Requires the process-local source path to still name the exact registered project source. */
        bool MatchesPrefabDropSource(const EditorWorkspaceViewModel &model, const AssetSceneDropRequest &request,
                                     const Assets::AssetRecord &record) {
            if (request.absoluteAssetPath.empty() || (request.target == AssetSceneDropTarget::Viewport && request.parent))
                return false;
            std::error_code error;
            const auto expected =
                std::filesystem::weakly_canonical(std::filesystem::path{model.projectRoot} / SourcePathFromUtf8(record.sourcePath.String()),
                                                  error);
            if (error)
                return false;
            const auto dragged = std::filesystem::weakly_canonical(SourcePathFromUtf8(request.absoluteAssetPath), error);
            return !error && dragged == expected;
        }
    }  // namespace

    void EditorWorkspaceController::HandleCreatePrimitive(const Runtime::PrimitiveId primitive, const std::optional<SceneObjectId> parent) {
        Result<SceneCommandResult> result = m_createSceneObject.Execute(PrimitiveCreationRequest{primitive, parent});
        if (result.HasError()) {
            HandleDocumentCommandResult(result, "Create object");
            return;
        }
        const SceneObjectId created = result.Value().object;
        const bool committed = result.Value().committed;
        HandleDocumentCommandResult(result, "Create object");
        if (committed) {
            m_viewModel.hierarchyRevealObject = created;
            m_viewModel.hierarchyRevealRevision = m_document.Revision();
            if (const Result<void> selected = m_selection.SetObjects({created}, created); selected.HasError())
                LOG_ERROR("editor.selection", "Select created object failed: %s", selected.ErrorValue().message.c_str());
            RefreshSelectionProjection();
        }
    }

    bool EditorWorkspaceController::ApplyAssetViewportPlacement(const AssetSceneDropRequest &request, const Math::Aabb &localBounds,
                                                                Math::Transform &localTransform, const bool publishFailure) const {
        if (request.target != AssetSceneDropTarget::Viewport)
            return true;

        const Result<AssetViewportPlacement> placement = ResolveAssetViewportPlacement(AssetViewportPlacementRequest{
            .scene = m_viewportScene,
            .normalizedX = request.normalizedX,
            .normalizedY = request.normalizedY,
            .aspect = request.aspect,
            .depthRange = request.depthRange,
            .localBounds = localBounds,
        });
        if (placement.HasError()) {
            if (publishFailure) {
                m_notifications.Publish("asset", NotificationSeverity::Error, placement.ErrorValue().message,
                                        Localized("workspace.asset_drop.place_failed_title", "Asset could not be placed"),
                                        "asset_drop_placement_failed");
            }
            return false;
        }
        localTransform.translation = placement.Value().worldPosition;
        if (!request.parent.has_value())
            return true;

        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active.has_value()) {
            if (publishFailure) {
                m_notifications.Publish("asset", NotificationSeverity::Warning,
                                        Localized("workspace.asset_drop.parent_missing", "The hierarchy target no longer exists."),
                                        Localized("workspace.asset_drop.not_added", "Asset not added"),
                                        "asset_drop_parent_runtime_missing");
            }
            return false;
        }
        const Result<SceneObjectWorldTransforms> parentWorld = ResolveSceneObjectWorldTransforms(*active, *request.parent);
        const Result<Math::Mat4> parentInverse = parentWorld.HasValue() ? Math::TryInverseAffine(parentWorld.Value().localToWorld)
                                                                        : Result<Math::Mat4>::Failure(parentWorld.ErrorValue());
        if (parentInverse.HasError()) {
            if (publishFailure) {
                m_notifications.Publish("asset", NotificationSeverity::Warning,
                                        Localized("workspace.asset_drop.parent_missing", "The hierarchy target no longer exists."),
                                        Localized("workspace.asset_drop.not_added", "Asset not added"),
                                        "asset_drop_parent_transform_invalid");
            }
            return false;
        }
        localTransform.translation = Math::TransformAffinePoint(parentInverse.Value(), placement.Value().worldPosition);
        return true;
    }

    /** @copydoc EditorWorkspaceController::AdmitPrefabDrop */
    bool EditorWorkspaceController::AdmitPrefabDrop(const AssetSceneDropRequest &request) const {
        const auto active = m_viewModel.workspacePanelHost.ActiveDocument();
        const bool prefabFocused = m_activePrefabDocument || std::ranges::any_of(m_prefabDocuments, [active](const auto &document) {
            return document->Instance() == active;
        });
        return !prefabFocused && request.fence == m_viewModel.assetDropFence && request.fence.session.IsValid() &&
               request.fence.replacementEpoch == m_document.ReplacementEpoch() && !m_playSession.IsActive() &&
               !m_viewModel.sceneExternalConflict && !m_viewModel.recoveryAvailable && m_mutations && m_prefabSourceCapture;
    }

    /** @copydoc EditorWorkspaceController::PreparePrefabDrop */
    Result<void> EditorWorkspaceController::PreparePrefabDrop(const AssetSceneDropRequest &request, const Assets::AssetRecord &record) {
        if (!AdmitPrefabDrop(request))
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
        if (m_prefabDrop && m_prefabDrop->asset == record.id && m_prefabDrop->fence == request.fence &&
            m_prefabDrop->registry == m_assetRegistry.Revision())
            return Result<void>::Success();
        m_prefabDrop.reset();
        auto limits = Prefab::PrefabLimitProfile::Create({});
        if (limits.HasError())
            return Result<void>::Failure(limits.ErrorValue());
        std::vector<Assets::AssetId> roots{record.id};
        for (const auto &instance : m_document.Snapshot().prefabInstances)
            roots.push_back(instance.sourcePrefab.Asset());
        auto captured = m_prefabSourceCapture(m_assetRegistry, roots, limits.Value(), m_prefabCancellation.Token());
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        const auto reference = Prefab::PrefabAssetReference::Create(record.id);
        if (reference.HasError())
            return Result<void>::Failure(reference.ErrorValue());
        SceneDocumentSnapshot detached{.revision = m_document.Revision(), .state = m_document.State()};
        detached.prefabInstances.push_back({Prefab::PrefabInstanceId::Create(1).Value(), reference.Value(), std::nullopt, {}});
        auto definition = ConvertSceneDocumentToRuntime(detached, m_previewSceneId, captured.Value().sources.resolver, limits.Value(),
                                                        nullptr, m_triggerBinding);
        if (definition.HasError())
            return Result<void>::Failure(definition.ErrorValue());
        auto geometry = PreparePrefabDropGeometry(definition.Value(), m_primitiveMeshCache);
        if (geometry.HasError())
            return Result<void>::Failure(geometry.ErrorValue());
        auto projectMetadata =
            Assets::CaptureAssetSource(m_viewModel.projectRoot, std::filesystem::path{m_viewModel.projectRoot} / ".horo/project.json",
                                       1024U * 1024U, m_prefabCancellation.Token());
        if (projectMetadata.HasError())
            return Result<void>::Failure(projectMetadata.ErrorValue());
        m_prefabDrop.emplace(request.fence, record.id, m_assetRegistry.Revision(), std::move(captured).Value().sources,
                             std::move(geometry).Value(), projectMetadata.Value().identity);
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ValidatePrefabDropPublication */
    Result<ProjectMutationLease> EditorWorkspaceController::ValidatePrefabDropPublication(const AssetSceneDropRequest &request) const {
        using Return = Result<ProjectMutationLease>;
        if (!m_prefabDrop || !AdmitPrefabDrop(request))
            return Return::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
        auto lease = m_mutations->TryAcquire({m_viewModel.projectRoot, ProjectMutationOwner::Asset, "editor.prefab.drop"});
        if (lease.HasError())
            return lease;
        if (const auto unchanged = m_prefabDrop->sources.inputs.VerifyUnchanged(m_prefabCancellation.Token()); unchanged.HasError())
            return Return::Failure(unchanged.ErrorValue());
        if (const auto projectMetadata =
                Assets::CaptureAssetSource(m_viewModel.projectRoot, std::filesystem::path{m_viewModel.projectRoot} / ".horo/project.json",
                                           1024U * 1024U, m_prefabCancellation.Token());
            projectMetadata.HasError() || projectMetadata.Value().identity != m_prefabDrop->projectMetadata)
            return Return::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
        if (m_defaultScenePath) {
            const auto current = InspectProjectSceneFingerprint(m_viewModel.projectRoot, *m_defaultScenePath);
            if (current.HasError() || !m_sceneFingerprint || current.Value() != *m_sceneFingerprint)
                return Return::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
        }
        return lease;
    }

    /** @copydoc EditorWorkspaceController::PlacePrefabAsset */
    void EditorWorkspaceController::PlacePrefabAsset(const AssetSceneDropRequest &request, const Assets::AssetRecord &record) {
        if (const auto prepared = PreparePrefabDrop(request, record); prepared.HasError()) {
            HandleInstantiatedAssetCommand(Result<SceneCommandResult>::Failure(prepared.ErrorValue()));
            m_prefabDrop.reset();
            return;
        }
        Math::Transform placement;
        if (!ApplyAssetViewportPlacement(request, m_prefabDrop->geometry.bounds, placement))
            return;
        auto validated = ValidatePrefabDropPublication(request);
        if (validated.HasError()) {
            HandleInstantiatedAssetCommand(Result<SceneCommandResult>::Failure(validated.ErrorValue()));
            m_prefabDrop.reset();
            return;
        }
        std::optional<ProjectMutationLease> publicationLease{std::move(validated).Value()};
        // One canonical command owns placement, dirty state, recovery revision and exactly one semantic history entry.
        const auto reference = Prefab::PrefabAssetReference::Create(record.id);
        if (reference.HasError())
            return;
        auto result = m_documentCommands.Execute(CreateScenePrefabInstanceCommand{reference.Value(), request.parent, placement});
        // Release the writer lease before derived hierarchy refresh can request a new source capture.
        publicationLease.reset();
        if (result.HasValue() && result.Value().committed)
            m_prefabSources.emplace(std::move(m_prefabDrop->sources));
        m_prefabDrop.reset();
        if (result.HasError() || !result.Value().committed) {
            HandleInstantiatedAssetCommand(result);
            return;
        }
        m_selection.Clear();
        m_viewModel.prefabSelection = PrefabHierarchyTarget{*result.Value().prefabInstance, std::nullopt};
        HandleDocumentCommandResult(result, "Place prefab");
        RefreshSelectionProjection();
    }

    void EditorWorkspaceController::PreviewAssetPlacement(const AssetSceneDropRequest &request) {
        if (request.target != AssetSceneDropTarget::Viewport && request.assetType != "core.prefab")
            return;
        if (m_assetPlacementPreviewActive) {
            static_cast<void>(ClearAssetViewportPlacementPreview(m_viewportScene));
            m_assetPlacementPreviewActive = false;
        }
        const Assets::AssetRecord *record = ResolveAssetDropRecord(request, false);
        if (record == nullptr)
            return;
        if (record->type.Value() == "core.prefab") {
            if (const auto prepared = PreparePrefabDrop(request, *record); prepared.HasError())
                return;
            if (request.target != AssetSceneDropTarget::Viewport)
                return;
            Math::Transform placement;
            if (!ApplyAssetViewportPlacement(request, m_prefabDrop->geometry.bounds, placement, false))
                return;
            // Viewport drops are world-root placements; hierarchy uses the same identity-local policy as final commit.
            if (const auto shown = ApplyPrefabDropPreview(m_viewportScene, m_prefabDrop->geometry, placement); shown.HasError())
                return;
            m_assetPlacementPreviewActive = true;
            ++m_viewportSceneRevision;
            return;
        }
        const std::filesystem::path source = ResolveAssetSourcePath(m_viewModel, request, *record);
        const auto loaded = m_assetMeshCache.Load(record->id, source);
        if (loaded.HasError())
            return;
        Math::Transform transform;
        if (!ApplyAssetViewportPlacement(request, loaded.Value().mesh->localBounds, transform, false))
            return;
        if (const Result<void> applied = ApplyAssetViewportPlacementPreview(m_viewportScene, loaded.Value(),
                                                                            AssetViewportPlacement{.worldPosition = transform.translation});
            applied.HasError()) {
            LOG_ERROR("editor.viewport", "Asset placement preview failed: %s", applied.ErrorValue().message.c_str());
            return;
        }
        m_assetPlacementPreviewActive = true;
        ++m_viewportSceneRevision;
    }

    void EditorWorkspaceController::CancelAssetPlacementPreview() {
        m_prefabDrop.reset();
        if (!m_assetPlacementPreviewActive)
            return;
        static_cast<void>(ClearAssetViewportPlacementPreview(m_viewportScene));
        m_assetPlacementPreviewActive = false;
        ++m_viewportSceneRevision;
    }

    void EditorWorkspaceController::HandleInstantiateAsset(const AssetSceneDropRequest &request) {
        // Retain source evidence until final revalidation; remove only the disposable render overlay.
        if (ClearAssetViewportPlacementPreview(m_viewportScene))
            ++m_viewportSceneRevision;
        m_assetPlacementPreviewActive = false;
        const Assets::AssetRecord *record = ResolveAssetDropRecord(request);
        if (record == nullptr) {
            m_prefabDrop.reset();
            return;
        }
        if (record->type.Value() == "core.prefab") {
            PlacePrefabAsset(request, *record);
            return;
        }

        const std::filesystem::path source = ResolveAssetSourcePath(m_viewModel, request, *record);
        const auto loaded = m_assetMeshCache.Load(record->id, source);
        if (loaded.HasError()) {
            m_notifications.Publish("asset", NotificationSeverity::Error, loaded.ErrorValue().message,
                                    Localized("workspace.asset_drop.load_failed_title", "Asset could not be loaded"),
                                    std::format("asset_drop_load_failed_{}", request.assetId));
            return;
        }

        Math::Transform localTransform;
        if (!ApplyAssetViewportPlacement(request, loaded.Value().mesh->localBounds, localTransform))
            return;
        std::string baseName = source.stem().string();
        if (baseName.empty())
            baseName = "Mesh";
        Result<SceneCommandResult> result =
            m_instantiateSceneAsset.Execute(AssetInstantiationRequest{record->id, std::move(baseName), request.parent, localTransform});
        HandleInstantiatedAssetCommand(result);
    }

    const Assets::AssetRecord *EditorWorkspaceController::ResolveAssetDropRecord(const AssetSceneDropRequest &request,
                                                                                 const bool publishFailure) const {
        const auto parsedId = Assets::AssetId::Parse(request.assetId);
        if (parsedId.HasError() || request.documentRevision != m_document.Revision()) {
            if (publishFailure) {
                m_notifications.Publish("asset", NotificationSeverity::Warning,
                                        Localized("workspace.asset_drop.stale",
                                                  "The asset drop was cancelled because the scene or drag reference changed."),
                                        Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_stale");
            }
            return nullptr;
        }
        const Assets::AssetRecord *record = m_assetRegistry.Find(parsedId.Value());
        if (record == nullptr || record->type.Value() != request.assetType) {
            if (publishFailure) {
                m_notifications.Publish("asset", NotificationSeverity::Error,
                                        Localized("workspace.asset_drop.missing",
                                                  "The dragged asset is no longer registered in this project."),
                                        Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_missing");
            }
            return nullptr;
        }
        if (record->type.Value() == "core.prefab" && !MatchesPrefabDropSource(m_viewModel, request, *record))
            return nullptr;
        if (!CanInstantiateAssetType(record->type.Value())) {
            if (publishFailure) {
                m_notifications.Publish("asset", NotificationSeverity::Info,
                                        Localized("workspace.asset_drop.unsupported",
                                                  "This asset type cannot be instantiated as a scene object."),
                                        Localized("workspace.asset_drop.unsupported_title", "Unsupported asset"),
                                        std::format("asset_drop_unsupported_{}", record->type.Value()));
            }
            return nullptr;
        }
        if (request.parent.has_value() && !m_document.Contains(*request.parent)) {
            if (publishFailure) {
                m_notifications.Publish("asset", NotificationSeverity::Warning,
                                        Localized("workspace.asset_drop.parent_missing", "The hierarchy target no longer exists."),
                                        Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_parent_missing");
            }
            return nullptr;
        }
        return record;
    }

    void EditorWorkspaceController::HandleInstantiatedAssetCommand(const Result<SceneCommandResult> &result) {
        if (result.HasError()) {
            const std::string message = result.ErrorValue().message;
            HandleDocumentCommandResult(result, "Instantiate asset");
            m_notifications.Publish("asset", NotificationSeverity::Error, message,
                                    Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_command_failed");
            return;
        }
        const SceneObjectId created = result.Value().object;
        const bool committed = result.Value().committed;
        HandleDocumentCommandResult(result, "Instantiate asset");
        if (!committed)
            return;
        m_viewModel.hierarchyRevealObject = created;
        m_viewModel.hierarchyRevealRevision = m_document.Revision();
        if (const Result<void> selected = m_selection.SetObjects({created}, created); selected.HasError())
            LOG_ERROR("editor.selection", "Select instantiated asset failed: %s", selected.ErrorValue().message.c_str());
        RefreshSelectionProjection();
        m_notifications.Publish("asset", NotificationSeverity::Success,
                                Localized("workspace.asset_drop.success", "Asset added to the scene."),
                                Localized("workspace.asset_drop.success_title", "Asset added"), "asset_drop_success");
    }

}  // namespace Horo::Editor
