#include "Horo/Runtime/Scene/RuntimeScene.h"

#include "Horo/Assets/AssetProvider.h"
#include "RuntimeSceneCommandApplier.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <format>
#include <limits>
#include <new>
#include <ranges>
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

    /** @copydoc SceneCommandBuffer::CreateGroup */
    Result<std::vector<DeferredEntity>> SceneCommandBuffer::CreateGroup(std::vector<RuntimeEntityGroupEntry> entries,
                                                                        std::vector<RuntimeGroupAssetLease> resources,
                                                                        const SceneStructuralAdmission &admission) {
        if (entries.empty() || entries.size() > 256 || resources.size() > 257 || !admission.scene.IsValid() ||
            admission.registry.value == 0 || entries.size() > std::numeric_limits<std::uint64_t>::max() - nextDeferred_)
            return Result<std::vector<DeferredEntity>>::Failure(
                MakeError(SceneErrors::StructuralCommitFailed, "Invalid structural group bounds."));
        std::size_t referenceCount{};
        std::size_t interfaceCount{};
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].parentInGroup.has_value() && (*entries[index].parentInGroup >= index || entries[index].info.parent))
                return Result<std::vector<DeferredEntity>>::Failure(
                    MakeError(SceneErrors::InvalidEntity, "Group parent must precede its child."));
            if (entries[index].physicsReferences.size() > 4096 - referenceCount)
                return Result<std::vector<DeferredEntity>>::Failure(MakeError(SceneErrors::InvalidEntity));
            referenceCount += entries[index].physicsReferences.size();
            if (entries[index].members.size() > 64 || entries[index].references.size() > 256 - interfaceCount)
                return Result<std::vector<DeferredEntity>>::Failure(MakeError(SceneErrors::InvalidEntity));
            interfaceCount += entries[index].references.size();
            if (entries[index].spawnLineage.size() > 16)
                return Result<std::vector<DeferredEntity>>::Failure(MakeError(SceneErrors::InvalidEntity));
            if (entries[index].spawnLineage != entries.front().spawnLineage)
                return Result<std::vector<DeferredEntity>>::Failure(MakeError(SceneErrors::InvalidEntity));
            for (std::size_t lineage = 0; lineage < entries[index].spawnLineage.size(); ++lineage) {
                const auto &asset = entries[index].spawnLineage[lineage];
                if (!asset.IsValid() || std::find(entries[index].spawnLineage.begin(),
                                                  entries[index].spawnLineage.begin() + static_cast<std::ptrdiff_t>(lineage),
                                                  asset) != entries[index].spawnLineage.begin() + static_cast<std::ptrdiff_t>(lineage))
                    return Result<std::vector<DeferredEntity>>::Failure(MakeError(SceneErrors::InvalidEntity));
            }
        }
        if (std::ranges::any_of(resources, [](const auto &pin) {
            return pin.artifact.Bytes().empty() || !pin.metadata.id.IsValid() || pin.metadata.expectedType.Value().empty();
        }))
            return Result<std::vector<DeferredEntity>>::Failure(MakeError(SceneErrors::AssetPayloadEmpty));
        std::vector<DeferredEntity> tokens;
        tokens.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index)
            tokens.emplace_back(nextDeferred_ + index);
        auto returned = tokens;
        commands_.emplace_back(CreateGroupCommand{std::move(entries), std::move(resources), std::move(tokens), admission});
        nextDeferred_ += returned.size();
        return Result<std::vector<DeferredEntity>>::Success(std::move(returned));
    }

    /** @copydoc SceneCommandBuffer::ValidateAdmission */
    Result<void> SceneCommandBuffer::ValidateAdmission(SceneRuntimeId scene, Assets::AssetRegistryRevision registry) const {
        for (const auto &command : commands_) {
            const SceneStructuralAdmission *admission = nullptr;
            if (const auto *group = std::get_if<CreateGroupCommand>(&command))
                admission = &group->admission;
            if (const auto *group = std::get_if<DestroyGroupCommand>(&command))
                admission = &group->admission;
            if (admission) {
                if (admission->cancellation.IsCancellationRequested() || admission->ownerCancellation.IsCancellationRequested() ||
                    admission->scopeCancellation.IsCancellationRequested())
                    return JobCancelled();
                if (admission->scene != scene)
                    return Result<void>::Failure(MakeError(SceneErrors::StaleEntity));
                if (std::holds_alternative<CreateGroupCommand>(command) && admission->registry != registry)
                    return Result<void>::Failure(MakeError(SceneErrors::AssetRevisionStale));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc SceneCommandBuffer::DestroyGroup */
    Result<void> SceneCommandBuffer::DestroyGroup(std::vector<EntityRef> entities, const SceneStructuralAdmission &admission) {
        if (entities.empty() || entities.size() > 256 || !admission.scene.IsValid())
            return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
        for (std::size_t index = 0; index < entities.size(); ++index) {
            if (!entities[index].IsValid() || entities[index].runtime != admission.scene ||
                std::find(entities.begin(), entities.begin() + static_cast<std::ptrdiff_t>(index), entities[index]) !=
                    entities.begin() + static_cast<std::ptrdiff_t>(index))
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
        }
        commands_.emplace_back(DestroyGroupCommand{std::move(entities), admission});
        return Result<void>::Success();
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
        for (auto &slot : clone->storage_.slots) {
            if (!slot.groupPhysicsReferences)
                continue;
            auto references = std::make_shared<std::vector<ResolvedGroupPhysicsBodyReference>>(*slot.groupPhysicsReferences);
            for (auto &reference : *references)
                reference.target.runtime = runtimeId;
            slot.groupPhysicsReferences = std::move(references);
        }
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
        slot.groupResources.reset();
        slot.groupReferences.reset();
        slot.groupSpawnLineage.reset();
        slot.groupPhysicsReferences.reset();
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

    /** @copydoc RuntimeScene::Commit */
    Result<StructuralCommitResult> RuntimeScene::Commit(const SceneCommandBuffer &commands) {
        return CommitWithRegistry(commands, nullptr);
    }

    /** @copydoc RuntimeScene::CommitWithRegistry */
    Result<StructuralCommitResult> RuntimeScene::CommitWithRegistry(
        const SceneCommandBuffer &commands, const Assets::AssetRegistry *registry,
        std::span<const std::unique_ptr<SceneStructuralParticipant>> participants, std::optional<Error> *notificationError) {
        if (commands.Empty())
            return Result<StructuralCommitResult>::Success({});
        const auto revision = registry ? registry->Snapshot().Revision() : Assets::AssetRegistryRevision{};
        if (auto admission = commands.ValidateAdmission(runtimeId_, revision); admission.HasError())
            return Result<StructuralCommitResult>::Failure(admission.ErrorValue());
        RuntimeSceneStorage candidate = storage_;
        StructuralCommitResult result;
        result.created.reserve(commands.commands_.size());
        const CommandApplier applier{*this, candidate, result};
        for (const SceneCommandBuffer::Command &command : commands.commands_) {
            if (const auto status = std::visit(applier, command); status.HasError())
                return Result<StructuralCommitResult>::Failure(status.ErrorValue());
        }
        std::vector<std::unique_ptr<SceneStructuralCandidate>> owners;
        if (const auto ready = applier.PrepareOwners(participants, owners); ready.HasError())
            return Result<StructuralCommitResult>::Failure(ready.ErrorValue());
        // Candidate preparation may allocate. Cancellation is sampled again only after all fallible work,
        // immediately before publication. Registry mutation is restricted to this same owner lane.
        const auto finalRevision = registry ? registry->Snapshot().Revision() : Assets::AssetRegistryRevision{};
        if (auto admission = commands.ValidateAdmission(runtimeId_, finalRevision); admission.HasError())
            return Result<StructuralCommitResult>::Failure(admission.ErrorValue());
        for (const auto &owner : owners)
            owner->Publish();
        storage_ = std::move(candidate);
        ++structuralRevision_;
        for (const auto &owner : owners) {
            if (auto notified = owner->AfterPublication(); notified.HasError() && notificationError && !*notificationError)
                *notificationError = std::move(notified).ErrorValue();
        }
        return Result<StructuralCommitResult>::Success(std::move(result));
    }

}  // namespace Horo::Runtime
