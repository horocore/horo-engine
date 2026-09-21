#include "Horo/Runtime/Scene/RuntimeScene.h"

#include "Horo/Assets/AssetProvider.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace Horo::Runtime {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &code, std::string message) {
            return Result<T>::Failure(MakeError(code, std::move(message)));
        }
    }  // namespace

    /** @copydoc SceneCommandBuffer::Create */
    DeferredEntity SceneCommandBuffer::Create(RuntimeEntityCreateInfo createInfo) {
        const DeferredEntity deferred{nextDeferred_++};
        commands_.emplace_back(CreateCommand{deferred, std::move(createInfo)});
        return deferred;
    }

    /** @copydoc SceneCommandBuffer::Destroy */
    void SceneCommandBuffer::Destroy(EntityRef entity) {
        commands_.emplace_back(DestroyCommand{entity});
    }

    /** @copydoc SceneCommandBuffer::SetLocalTransform */
    void SceneCommandBuffer::SetLocalTransform(const EntityRef entity, Math::Transform localTransform) {
        commands_.emplace_back(SetLocalTransformCommand{entity, std::move(localTransform)});
    }

    /** @copydoc SceneCommandBuffer::Empty */
    bool SceneCommandBuffer::Empty() const noexcept {
        return commands_.empty();
    }

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
        return RuntimeEntityView{entity, value.authoredObject, parent, &value.localTransform, &value.primitiveMesh, &value.components};
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
            return Failure<RuntimeEntityView>(SceneErrors::StaleView, "Scene view was invalidated by a structural commit.");
        if (!scene_->IsValid(scene_->storage_, entity))
            return Failure<RuntimeEntityView>(SceneErrors::StaleEntity, "Entity reference is stale or belongs to another runtime scene.");
        return Result<RuntimeEntityView>::Success(*EntityAt(entity.entity.index));
    }

    /** @copydoc RuntimeScene::RuntimeScene */
    RuntimeScene::RuntimeScene(const SceneRuntimeId runtimeId, const SceneDefinitionId definitionId, const SceneDefinitionRevision revision,
                               const RuntimeSceneConfig config, const Assets::AssetRegistryRevision assetRevision,
                               std::vector<ResolvedAsset> assets) noexcept
        : runtimeId_(runtimeId), definitionId_(definitionId), definitionRevision_(revision), assetRegistryRevision_(assetRevision),
          assets_(std::move(assets)), config_(config) {}

    /** @copydoc RuntimeScene::Create */
    Result<std::unique_ptr<RuntimeScene>> RuntimeScene::Create(const RuntimeSceneDefinition &definition, const SceneRuntimeId runtimeId,
                                                               const RuntimeSceneConfig config) {
        if (!definition.AssetDependencies().empty())
            return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::AssetServicesUnavailable,
                                                          "Asset-bearing definitions must be prepared through RuntimeSceneService.");
        return CreateResolved(definition, runtimeId, config, {}, {});
    }

    Result<std::unique_ptr<RuntimeScene>> RuntimeScene::CreateResolved(const RuntimeSceneDefinition &definition,
                                                                       const SceneRuntimeId runtimeId, const RuntimeSceneConfig config,
                                                                       const Assets::AssetRegistryRevision assetRevision,
                                                                       std::vector<ResolvedAsset> assets) {
        if (!runtimeId.IsValid() || config.maximumGeneration == 0)
            return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::InvalidCandidate,
                                                          "Runtime identity and maximum generation must be non-zero.");
        if (assets.size() != definition.AssetDependencies().size())
            return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::InvalidCandidate,
                                                          "Resolved asset set does not match the definition.");
        auto scene =
            std::make_unique<RuntimeScene>(runtimeId, definition.Id(), definition.Revision(), config, assetRevision, std::move(assets));
        scene->storage_.slots.reserve(definition.Entities().size());
        scene->storage_.authoredIndex.reserve(definition.Entities().size());

        for (const RuntimeEntityDefinition &entity : definition.Entities()) {
            RuntimeEntityCreateInfo info{entity.localTransform, std::nullopt, entity.object, entity.primitiveMesh, entity.components};
            Result<EntityRef> created = scene->CreateEntity(scene->storage_, info);
            if (created.HasError())
                return Result<std::unique_ptr<RuntimeScene>>::Failure(created.ErrorValue());
        }
        for (const RuntimeEntityDefinition &entity : definition.Entities()) {
            if (!entity.parent)
                continue;
            const std::optional<EntityRef> child = scene->View().Find(entity.object);
            const std::optional<EntityRef> parent = scene->View().Find(*entity.parent);
            if (!child || !parent)
                return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::ParentNotFound,
                                                              "Definition parent resolution failed during instantiation.");
            scene->storage_.slots[child->entity.index].parent = parent->entity;
        }
        return Result<std::unique_ptr<RuntimeScene>>::Success(std::move(scene));
    }

    /** @copydoc RuntimeSceneService::CloneActive */
    Result<std::unique_ptr<RuntimeScene>> RuntimeSceneService::CloneActive(const SceneRuntimeId runtimeId) const {
        if (!active_.scene || !runtimeId.IsValid() || runtimeId == active_.scene->runtimeId_)
            return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::InvalidCandidate,
                                                          "An active scene and a distinct runtime identity are required.");
        auto clone = std::make_unique<RuntimeScene>(runtimeId, active_.scene->definitionId_, active_.scene->definitionRevision_,
                                                    active_.scene->config_, active_.scene->assetRegistryRevision_, active_.scene->assets_);
        clone->storage_ = active_.scene->storage_;
        return Result<std::unique_ptr<RuntimeScene>>::Success(std::move(clone));
    }

    /** @copydoc RuntimeScene::View */
    RuntimeSceneView RuntimeScene::View() const noexcept {
        return RuntimeSceneView{*this};
    }

    Result<EntityRef> RuntimeScene::CreateEntity(RuntimeSceneStorage &storage, const RuntimeEntityCreateInfo &info) const {
        RuntimeEntityDefinition validation{info.authoredObject.value_or(SceneObjectId{1}), std::nullopt, info.localTransform,
                                           info.primitiveMesh, info.components};
        if (const Result<void> valid = ValidateRuntimeEntityDefinition(validation); valid.HasError())
            return Result<EntityRef>::Failure(valid.ErrorValue());
        if (info.parent && !IsValid(storage, *info.parent))
            return Failure<EntityRef>(SceneErrors::StaleEntity, "New entity parent reference is stale.");
        if (info.authoredObject && std::ranges::find(storage.authoredIndex, *info.authoredObject, [](const auto &entry) {
            return entry.first;
        }) != storage.authoredIndex.end())
            return Failure<EntityRef>(SceneErrors::DuplicateObject, "Runtime authored object identity already exists.");

        std::uint32_t index{};
        if (!storage.freeList.empty()) {
            index = storage.freeList.back();
            storage.freeList.pop_back();
        } else {
            if (storage.slots.size() >= std::numeric_limits<std::uint32_t>::max())
                return Failure<EntityRef>(SceneErrors::StructuralCommitFailed, "Runtime entity slot capacity is exhausted.");
            index = static_cast<std::uint32_t>(storage.slots.size());
            storage.slots.emplace_back();
        }
        Slot &slot = storage.slots[index];
        slot.active = true;
        slot.retired = false;
        slot.authoredObject = info.authoredObject;
        slot.parent = info.parent ? std::optional<EntityId>{info.parent->entity} : std::nullopt;
        slot.localTransform = info.localTransform;
        slot.primitiveMesh = info.primitiveMesh;
        slot.components = info.components;
        const EntityId id{index, slot.generation};
        if (slot.authoredObject)
            storage.authoredIndex.emplace_back(*slot.authoredObject, id);
        return Result<EntityRef>::Success(EntityRef{runtimeId_, id});
    }

    Result<void> RuntimeScene::DestroyEntity(RuntimeSceneStorage &storage, const EntityRef entity) const {
        if (!IsValid(storage, entity))
            return Result<void>::Failure(
                MakeError(SceneErrors::StaleEntity, "Destroyed entity reference is stale or belongs to another runtime."));
        if (std::ranges::any_of(storage.slots, [&](const Slot &slot) {
            return slot.active && slot.parent && *slot.parent == entity.entity;
        }))
            return Result<void>::Failure(
                MakeError(SceneErrors::StructuralCommitFailed, "Destroy children before destroying their runtime parent."));

        Slot &slot = storage.slots[entity.entity.index];
        if (slot.authoredObject)
            std::erase_if(storage.authoredIndex, [&slot](const auto &entry) {
                return entry.first == *slot.authoredObject;
            });
        slot.active = false;
        slot.authoredObject.reset();
        slot.parent.reset();
        slot.primitiveMesh.reset();
        slot.components = {};
        if (slot.generation == config_.maximumGeneration)
            slot.retired = true;
        else {
            ++slot.generation;
            storage.freeList.push_back(entity.entity.index);
        }
        return Result<void>::Success();
    }

    bool RuntimeScene::IsValid(const RuntimeSceneStorage &storage, const EntityRef entity) const noexcept {
        return entity.runtime == runtimeId_ && entity.entity.IsValid() && entity.entity.index < storage.slots.size() &&
               storage.slots[entity.entity.index].active && storage.slots[entity.entity.index].generation == entity.entity.generation;
    }

    struct RuntimeScene::CommandApplier {
        const RuntimeScene &scene;
        RuntimeSceneStorage &candidate;
        StructuralCommitResult &result;

        Result<void> operator()(const SceneCommandBuffer::CreateCommand &create) const {
            const Result<EntityRef> created = scene.CreateEntity(candidate, create.info);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            result.created.emplace_back(create.deferred, created.Value());
            return Result<void>::Success();
        }

        Result<void> operator()(const SceneCommandBuffer::DestroyCommand &destroy) const {
            if (const Result<void> destroyedResult = scene.DestroyEntity(candidate, destroy.entity); destroyedResult.HasError())
                return Result<void>::Failure(destroyedResult.ErrorValue());
            ++result.destroyed;
            return Result<void>::Success();
        }

        Result<void> operator()(const SceneCommandBuffer::SetLocalTransformCommand &transform) const {
            if (!scene.IsValid(candidate, transform.entity) || transform.localTransform.TryToMatrix().HasError())
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity, "Deferred transform target or value is invalid."));
            candidate.slots[transform.entity.entity.index].localTransform = transform.localTransform;
            ++result.transformsUpdated;
            return Result<void>::Success();
        }
    };

    /** @copydoc RuntimeScene::Commit */
    Result<StructuralCommitResult> RuntimeScene::Commit(const SceneCommandBuffer &commands) {
        if (commands.Empty())
            return Result<StructuralCommitResult>::Success({});
        RuntimeSceneStorage candidate = storage_;
        StructuralCommitResult result;
        result.created.reserve(commands.commands_.size());
        const CommandApplier applier{*this, candidate, result};
        for (const SceneCommandBuffer::Command &command : commands.commands_) {
            if (const auto status = std::visit(applier, command); status.HasError())
                return Result<StructuralCommitResult>::Failure(status.ErrorValue());
        }
        storage_ = std::move(candidate);
        ++structuralRevision_;
        return Result<StructuralCommitResult>::Success(std::move(result));
    }

}  // namespace Horo::Runtime
