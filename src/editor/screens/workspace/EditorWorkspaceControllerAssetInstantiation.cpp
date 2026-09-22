#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <format>
#include <optional>
#include <string>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] std::filesystem::path ResolveAssetSourcePath(const EditorWorkspaceViewModel &viewModel,
                                                                   const AssetSceneDropRequest &request,
                                                                   const Assets::AssetRecord &record) {
            const std::filesystem::path draggedPath{request.absoluteAssetPath};
            if (draggedPath.is_absolute())
                return draggedPath;
            return std::filesystem::path{viewModel.projectRoot} / record.sourcePath.String();
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

    void EditorWorkspaceController::PreviewAssetPlacement(const AssetSceneDropRequest &request) {
        if (request.target != AssetSceneDropTarget::Viewport)
            return;
        if (m_assetPlacementPreviewActive) {
            static_cast<void>(ClearAssetViewportPlacementPreview(m_viewportScene));
            m_assetPlacementPreviewActive = false;
        }
        const Assets::AssetRecord *record = ResolveAssetDropRecord(request, false);
        if (record == nullptr)
            return;
        const std::filesystem::path source = ResolveAssetSourcePath(m_viewModel, request, *record);
        const auto loaded = m_assetMeshCache.Load(record->id, source);
        if (loaded.HasError())
            return;
        Math::Transform transform;
        if (!ApplyAssetViewportPlacement(request, loaded.Value().mesh->localBounds, transform, false))
            return;
        const Result<void> applied = ApplyAssetViewportPlacementPreview(m_viewportScene, loaded.Value(),
                                                                        AssetViewportPlacement{.worldPosition = transform.translation});
        if (applied.HasError()) {
            LOG_ERROR("editor.viewport", "Asset placement preview failed: %s", applied.ErrorValue().message.c_str());
            return;
        }
        m_assetPlacementPreviewActive = true;
        ++m_viewportSceneRevision;
    }

    void EditorWorkspaceController::CancelAssetPlacementPreview() {
        if (!m_assetPlacementPreviewActive)
            return;
        static_cast<void>(ClearAssetViewportPlacementPreview(m_viewportScene));
        m_assetPlacementPreviewActive = false;
        ++m_viewportSceneRevision;
    }

    void EditorWorkspaceController::HandleInstantiateAsset(const AssetSceneDropRequest &request) {
        CancelAssetPlacementPreview();
        const Assets::AssetRecord *record = ResolveAssetDropRecord(request);
        if (record == nullptr)
            return;

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
