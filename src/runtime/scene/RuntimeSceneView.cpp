#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>

namespace Horo::Runtime {
    /** @copydoc RuntimeSceneView::RuntimeSceneView */
    RuntimeSceneView::RuntimeSceneView(const RuntimeScene &scene) noexcept
        : scene_(&scene), structuralRevision_(scene.structuralRevision_) {}

    /** @copydoc RuntimeSceneView::IsCurrent */
    bool RuntimeSceneView::IsCurrent() const noexcept {
        return scene_ != nullptr && structuralRevision_ == scene_->structuralRevision_;
    }

    /** @copydoc RuntimeSceneView::RuntimeId */
    SceneRuntimeId RuntimeSceneView::RuntimeId() const noexcept {
        return scene_ ? scene_->runtimeId_ : SceneRuntimeId{};
    }

    /** @copydoc RuntimeSceneView::DefinitionId */
    SceneDefinitionId RuntimeSceneView::DefinitionId() const noexcept {
        return scene_ ? scene_->definitionId_ : SceneDefinitionId{};
    }

    /** @copydoc RuntimeSceneView::DefinitionRevision */
    SceneDefinitionRevision RuntimeSceneView::DefinitionRevision() const noexcept {
        return scene_ ? scene_->definitionRevision_ : SceneDefinitionRevision{};
    }

    /** @copydoc RuntimeSceneView::AssetRegistryRevision */
    Assets::AssetRegistryRevision RuntimeSceneView::AssetRegistryRevision() const noexcept {
        return scene_ ? scene_->assetRegistryRevision_ : Assets::AssetRegistryRevision{};
    }

    /** @copydoc RuntimeSceneView::SlotCount */
    std::size_t RuntimeSceneView::SlotCount() const noexcept {
        return IsCurrent() ? scene_->storage_.slots.size() : 0;
    }

    /** @copydoc RuntimeSceneView::EntityAt */
    std::optional<RuntimeEntityView> RuntimeSceneView::EntityAt(const std::size_t slot) const noexcept {
        if (!IsCurrent() || slot >= scene_->storage_.slots.size() || !scene_->storage_.slots[slot].active)
            return std::nullopt;
        const RuntimeScene::Slot &value = scene_->storage_.slots[slot];
        const EntityRef entity{scene_->runtimeId_, EntityId{static_cast<std::uint32_t>(slot), value.generation}};
        std::optional<EntityRef> parent;
        if (value.parent)
            parent = EntityRef{scene_->runtimeId_, *value.parent};
        return RuntimeEntityView{entity,
                                 value.authoredObject,
                                 parent,
                                 &value.localTransform,
                                 &value.primitiveMesh,
                                 &value.components,
                                 value.groupPhysicsReferences
                                     ? std::span<const ResolvedGroupPhysicsBodyReference>{*value.groupPhysicsReferences}
                                     : std::span<const ResolvedGroupPhysicsBodyReference>{},
                                 value.groupResources ? std::span<const RuntimeGroupAssetLease>{*value.groupResources}
                                                      : std::span<const RuntimeGroupAssetLease>{}};
    }

    /** @copydoc RuntimeSceneView::Find */
    std::optional<EntityRef> RuntimeSceneView::Find(const SceneObjectId object) const noexcept {
        if (!IsCurrent() || !object.IsValid())
            return std::nullopt;
        const auto found = std::ranges::find(scene_->storage_.authoredIndex, object, [](const auto &entry) {
            return entry.first;
        });
        if (found == scene_->storage_.authoredIndex.end())
            return std::nullopt;
        return EntityRef{scene_->runtimeId_, found->second};
    }

    /** @copydoc RuntimeSceneView::FindAsset */
    std::optional<RuntimeSceneAssetView> RuntimeSceneView::FindAsset(const Assets::AssetId id) const noexcept {
        if (!IsCurrent() || !id.IsValid())
            return std::nullopt;
        const auto found = std::ranges::find(scene_->assets_, id, [](const RuntimeScene::ResolvedAsset &asset) {
            return asset.dependency.id;
        });
        if (found == scene_->assets_.end() || !found->payload)
            return std::nullopt;
        return RuntimeSceneAssetView{found->dependency.id, &found->dependency.expectedType, std::span<const std::uint8_t>{*found->payload}};
    }

    /** @copydoc RuntimeSceneView::Get */
    Result<RuntimeEntityView> RuntimeSceneView::Get(const EntityRef entity) const {
        if (!IsCurrent())
            return Result<RuntimeEntityView>::Failure(
                MakeError(SceneErrors::StaleView, "Scene view was invalidated by a structural commit."));
        if (!scene_->IsValid(scene_->storage_, entity))
            return Result<RuntimeEntityView>::Failure(
                MakeError(SceneErrors::StaleEntity, "Entity reference is stale or belongs to another runtime scene."));
        return Result<RuntimeEntityView>::Success(*EntityAt(entity.entity.index));
    }

}  // namespace Horo::Runtime
