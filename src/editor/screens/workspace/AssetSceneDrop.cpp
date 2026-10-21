#include "editor/screens/workspace/AssetSceneDrop.h"

#include "editor/renderer/EditorViewportScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace Horo::Editor {
    namespace {
        template <std::size_t Size> void CopyBounded(std::array<char, Size> &destination, const std::string_view source) noexcept {
            const std::size_t count = std::min(source.size(), Size - 1);
            std::memcpy(destination.data(), source.data(), count);
            destination[count] = '\0';
        }

        template <std::size_t Size> [[nodiscard]] bool IsTerminated(const std::array<char, Size> &value) noexcept {
            return std::ranges::find(value, '\0') != value.end();
        }

        [[nodiscard]] float Snap(const float value, const float step) noexcept {
            return std::round(value / step) * step;
        }

        constexpr SceneObjectId AssetPlacementPreviewObject{std::numeric_limits<std::uint64_t>::max()};
    }  // namespace

    AssetSceneDragPayload MakeAssetSceneDragPayload(const std::string_view assetId, const std::string_view assetType,
                                                    const std::string_view absolutePath, const bool registered) noexcept {
        AssetSceneDragPayload payload;
        CopyBounded(payload.assetId, assetId);
        CopyBounded(payload.assetType, assetType);
        CopyBounded(payload.absolutePath, absolutePath);
        payload.registered = registered && assetId.size() < payload.assetId.size() && assetType.size() < payload.assetType.size() &&
                             absolutePath.size() < payload.absolutePath.size();
        return payload;
    }

    bool CanInstantiateAssetType(const std::string_view assetType) noexcept {
        return assetType == "core.mesh" || assetType == "core.prefab";
    }

    AssetSceneDropPolicyResult EvaluateAssetSceneDrop(const AssetSceneDragPayload &payload) noexcept {
        using enum AssetSceneDropRejection;
        if (!IsTerminated(payload.assetId) || !IsTerminated(payload.assetType) || !IsTerminated(payload.absolutePath)) {
            return {false, InvalidPayload};
        }
        if (!payload.registered || payload.assetId.front() == '\0')
            return {false, Unregistered};
        if (const auto parsed = Assets::AssetId::Parse(payload.assetId.data()); parsed.HasError())
            return {false, InvalidPayload};
        if (!CanInstantiateAssetType(payload.assetType.data()))
            return {false, UnsupportedType};
        return {true, None};
    }

    Result<AssetViewportPlacement> ResolveAssetViewportPlacement(const AssetViewportPlacementRequest &request) {
        if (!request.scene.View().IsValid() || !std::isfinite(request.normalizedX) || !std::isfinite(request.normalizedY) ||
            !std::isfinite(request.aspect) || request.normalizedX < 0.0F || request.normalizedX > 1.0F || request.normalizedY < 0.0F ||
            request.normalizedY > 1.0F || request.aspect <= 0.0F || !request.localBounds.IsValid() ||
            !std::isfinite(request.fallbackDistance) || request.fallbackDistance <= 0.0F ||
            (request.snapToGrid && (!std::isfinite(request.gridStep) || request.gridStep <= 0.0F))) {
            return Result<AssetViewportPlacement>::Failure(Error{.message = "Asset viewport placement request is invalid."});
        }

        const Result<Math::Ray> ray =
            BuildEditorViewportRay(request.scene.camera, request.normalizedX, request.normalizedY, request.aspect, request.depthRange);
        if (ray.HasError())
            return Result<AssetViewportPlacement>::Failure(ray.ErrorValue());

        float nearest = std::numeric_limits<float>::max();
        for (std::size_t index = 0; index < request.scene.instances.size(); ++index) {
            if (index < request.scene.instanceObjects.size() && request.scene.instanceObjects[index] == AssetPlacementPreviewObject)
                continue;
            const auto &instance = request.scene.instances[index];
            const Result<Math::Aabb> worldBounds = Math::TransformAabb(instance.localBounds, instance.localToWorld);
            if (worldBounds.HasError())
                return Result<AssetViewportPlacement>::Failure(worldBounds.ErrorValue());
            const Result<std::optional<Math::RayHit>> hit = Math::IntersectRayAabb(ray.Value(), worldBounds.Value());
            if (hit.HasError())
                return Result<AssetViewportPlacement>::Failure(hit.ErrorValue());
            if (hit.Value().has_value())
                nearest = std::min(nearest, hit.Value()->distance);
        }

        AssetViewportPlacement result;
        if (nearest != std::numeric_limits<float>::max()) {
            result.worldPosition = ray.Value().origin + ray.Value().direction * nearest;
            result.kind = AssetViewportPlacementKind::Surface;
        } else if (std::fabs(ray.Value().direction.y) > Math::DefaultEpsilon) {
            const float distance = -ray.Value().origin.y / ray.Value().direction.y;
            if (distance > 0.0F) {
                result.worldPosition = ray.Value().origin + ray.Value().direction * distance;
                result.kind = AssetViewportPlacementKind::GroundPlane;
            } else {
                result.worldPosition = ray.Value().origin + ray.Value().direction * request.fallbackDistance;
            }
        } else {
            result.worldPosition = ray.Value().origin + ray.Value().direction * request.fallbackDistance;
        }

        result.worldPosition.y -= request.localBounds.minimum.y;
        if (request.snapToGrid) {
            result.worldPosition.x = Snap(result.worldPosition.x, request.gridStep);
            result.worldPosition.y = Snap(result.worldPosition.y, request.gridStep);
            result.worldPosition.z = Snap(result.worldPosition.z, request.gridStep);
        }
        return Result<AssetViewportPlacement>::Success(result);
    }

    HierarchyAssetDropPlacement ResolveHierarchyAssetDropPlacement(const float normalizedRowY, const SceneObjectId hoveredObject,
                                                                   const std::optional<SceneObjectId> hoveredParent) noexcept {
        constexpr float siblingEdgeRatio = 0.25F;
        using enum AssetSceneDropTarget;
        using enum HierarchyAssetDropZone;
        if (normalizedRowY <= siblingEdgeRatio)
            return {hoveredParent, HierarchySibling, BeforeSibling};
        if (normalizedRowY >= 1.0F - siblingEdgeRatio)
            return {hoveredParent, HierarchySibling, AfterSibling};
        return {hoveredObject, HierarchyChild, Child};
    }

    bool ClearAssetViewportPlacementPreview(EditorViewportSceneSnapshot &scene) noexcept {
        if (scene.instances.size() != scene.instanceObjects.size() || scene.instances.size() != scene.instancePickable.size())
            return false;

        bool removed = false;
        for (std::size_t index = scene.instanceObjects.size(); index > 0; --index) {
            const std::size_t candidate = index - 1;
            if (scene.instanceObjects[candidate] != AssetPlacementPreviewObject)
                continue;
            scene.instances.erase(scene.instances.begin() + static_cast<std::ptrdiff_t>(candidate));
            scene.instanceObjects.erase(scene.instanceObjects.begin() + static_cast<std::ptrdiff_t>(candidate));
            scene.instancePickable.erase(scene.instancePickable.begin() + static_cast<std::ptrdiff_t>(candidate));
            removed = true;
        }
        if (removed) {
            std::erase_if(scene.meshResources, [&scene](const auto &resource) {
                return std::ranges::none_of(scene.instances, [&resource](const auto &instance) {
                    return instance.mesh == resource.handle;
                });
            });
            std::erase_if(scene.meshLeases, [&scene](const auto &lease) {
                return std::ranges::none_of(scene.meshResources, [&lease](const auto &resource) {
                    return resource.handle == Render::RenderMeshSourceHandle{lease.Id(), 1};
                });
            });
        }
        return removed;
    }

    /** @copydoc PreparePrefabDropGeometry */
    Result<PrefabDropGeometry> PreparePrefabDropGeometry(const Runtime::RuntimeSceneDefinition &definition,
                                                         Runtime::PrimitiveMeshCache &meshes) {
        PrefabDropGeometry result;
        std::map<Runtime::SceneObjectId, Math::Mat4> worlds;
        bool hasBounds = false;
        for (const auto &entity : definition.Entities()) {
            Math::Mat4 world = entity.localTransform.ToMatrix();
            if (entity.parent) {
                const auto parent = worlds.find(*entity.parent);
                if (parent == worlds.end())
                    return Result<PrefabDropGeometry>::Failure(Error{.message = "Prefab preview parent order is invalid."});
                world = Math::Multiply(parent->second, world);
            }
            worlds.try_emplace(entity.object, world);
            if (!entity.primitiveMesh)
                continue;
            auto acquired = meshes.Acquire(*entity.primitiveMesh);
            if (acquired.HasError())
                return Result<PrefabDropGeometry>::Failure(acquired.ErrorValue());
            auto lease = std::move(acquired).Value();
            auto bounds = Math::TransformAabb(lease.Data().localBounds, world);
            if (bounds.HasError())
                return Result<PrefabDropGeometry>::Failure(bounds.ErrorValue());
            if (!hasBounds) {
                result.bounds = bounds.Value();
                hasBounds = true;
            } else {
                const auto &other = bounds.Value();
                result.bounds.minimum = {std::min(result.bounds.minimum.x, other.minimum.x),
                                         std::min(result.bounds.minimum.y, other.minimum.y),
                                         std::min(result.bounds.minimum.z, other.minimum.z)};
                result.bounds.maximum = {std::max(result.bounds.maximum.x, other.maximum.x),
                                         std::max(result.bounds.maximum.y, other.maximum.y),
                                         std::max(result.bounds.maximum.z, other.maximum.z)};
            }
            result.instances.emplace_back(Render::RenderMeshSourceHandle{lease.Id(), 1}, world, lease.Data().localBounds,
                                          Render::CoreDefaultMaterial);
            result.leases.push_back(std::move(lease));
        }
        // Empty/light-only prefabs still expose a visible placement marker, never authored geometry.
        if (result.instances.empty()) {
            auto marker = meshes.Acquire(Runtime::PrimitiveMeshDescriptor{});
            if (marker.HasError())
                return Result<PrefabDropGeometry>::Failure(marker.ErrorValue());
            auto lease = std::move(marker).Value();
            result.instances.emplace_back(Render::RenderMeshSourceHandle{lease.Id(), 1}, Math::Mat4::Identity(), lease.Data().localBounds,
                                          Render::CoreDefaultMaterial);
            result.leases.push_back(std::move(lease));
        }
        return Result<PrefabDropGeometry>::Success(std::move(result));
    }

    /** @copydoc ApplyPrefabDropPreview */
    Result<void> ApplyPrefabDropPreview(EditorViewportSceneSnapshot &scene, const PrefabDropGeometry &geometry,
                                        const Math::Transform &placement) {
        if (!Math::IsFinite(placement.translation) || scene.instances.size() != scene.instanceObjects.size() ||
            scene.instances.size() != scene.instancePickable.size())
            return Result<void>::Failure(Error{.message = "Prefab preview placement is invalid."});
        static_cast<void>(ClearAssetViewportPlacementPreview(scene));
        for (const auto &lease : geometry.leases) {
            const Render::RenderMeshSourceHandle handle{lease.Id(), 1};
            if (std::ranges::find(scene.meshResources, handle, &EditorViewportMeshResourceView::handle) == scene.meshResources.end()) {
                scene.meshResources.emplace_back(handle, lease.Data().vertices, lease.Data().indices, lease.Data().localBounds);
                scene.meshLeases.push_back(lease);
            }
        }
        for (auto instance : geometry.instances) {
            instance.localToWorld = Math::Multiply(placement.ToMatrix(), instance.localToWorld);
            scene.instances.push_back(instance);
            scene.instanceObjects.push_back(AssetPlacementPreviewObject);
            scene.instancePickable.push_back(0U);
        }
        return Result<void>::Success();
    }

    Result<void> ApplyAssetViewportPlacementPreview(EditorViewportSceneSnapshot &scene, const EditorAssetMeshView &mesh,
                                                    const AssetViewportPlacement &placement) {
        static_cast<void>(ClearAssetViewportPlacementPreview(scene));
        if (mesh.mesh == nullptr || !mesh.handle.IsValid() || !mesh.mesh->IsValid() || !Math::IsFinite(placement.worldPosition))
            return Result<void>::Failure(Error{.message = "Asset placement preview is invalid."});
        if (scene.instances.size() != scene.instanceObjects.size() || scene.instances.size() != scene.instancePickable.size())
            return Result<void>::Failure(Error{.message = "Viewport scene identity data is inconsistent."});

        if (std::ranges::find(scene.meshResources, mesh.handle, &EditorViewportMeshResourceView::handle) == scene.meshResources.end()) {
            scene.meshResources.emplace_back(mesh.handle, mesh.mesh->vertices, mesh.mesh->indices, mesh.mesh->localBounds);
        }
        const Math::Transform transform{.translation = placement.worldPosition};
        scene.instances.emplace_back(mesh.handle, transform.ToMatrix(), mesh.mesh->localBounds, Render::CoreDefaultMaterial,
                                     Render::RenderInstancePresentation{
                                         .tint = {0.10F, 0.72F, 1.0F},
                                         .tintStrength = 0.62F,
                                     });
        scene.instanceObjects.push_back(AssetPlacementPreviewObject);
        scene.instancePickable.push_back(0U);
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
