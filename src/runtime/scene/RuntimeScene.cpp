#include "Horo/Runtime/Scene/RuntimeScene.h"

#include "Horo/Assets/AssetProvider.h"
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

        Result<void> operator()(const SceneCommandBuffer::DestroyGroupCommand &group) const {
            const auto root = group.entities.front();
            if (!scene.IsValid(candidate, root))
                return Result<void>::Failure(MakeError(SceneErrors::StaleEntity));
            const auto resources = candidate.slots[root.entity.index].groupResources;
            if (!resources)
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            const auto count = std::ranges::count_if(candidate.slots, [&resources](const auto &slot) {
                return slot.active && slot.groupResources == resources;
            });
            if (static_cast<std::size_t>(count) != group.entities.size())
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            for (const auto entity : group.entities) {
                if (!scene.IsValid(candidate, entity) || candidate.slots[entity.entity.index].groupResources != resources)
                    return Result<void>::Failure(MakeError(SceneErrors::StaleEntity));
            }
            for (const auto entity : group.entities | std::views::reverse) {
                if (auto destroyed = scene.DestroyEntity(candidate, entity); destroyed.HasError())
                    return destroyed;
                ++result.destroyed;
            }
            return Result<void>::Success();
        }

        Result<void> operator()(const SceneCommandBuffer::CreateGroupCommand &group) const {
            if (group.admission.scene != scene.runtimeId_ || group.admission.cancellation.IsCancellationRequested() ||
                group.admission.ownerCancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(SceneErrors::StaleEntity));
            auto resources = std::make_shared<const std::vector<RuntimeGroupAssetLease>>(group.resources);
            auto lineage = std::make_shared<const std::vector<Assets::AssetId>>(group.entries.front().spawnLineage);
            std::vector<EntityRef> entities;
            entities.reserve(group.entries.size());
            for (std::size_t index = 0; index < group.entries.size(); ++index) {
                auto info = group.entries[index].info;
                if (group.entries[index].parentInGroup.has_value())
                    info.parent = entities[*group.entries[index].parentInGroup];
                auto created = scene.CreateEntity(candidate, info);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                entities.push_back(created.Value());
                candidate.slots[created.Value().entity.index].groupResources = resources;
                candidate.slots[created.Value().entity.index].groupSpawnLineage = lineage;
                result.created.emplace_back(group.deferred[index], created.Value());
            }
            if (auto resolved = ResolveInterfaces(group, entities); resolved.HasError())
                return resolved;
            return ResolveGroupReferences(group, entities);
        }

        /** @brief Validates typed external relationships against the candidate without keeping targets alive. */
        [[nodiscard]] bool HasExternal(const RuntimeGroupExternalReference &target) const {
            if (!scene.IsValid(candidate, target.entity))
                return false;
            if (!target.component)
                return true;
            return std::ranges::any_of(candidate.slots[target.entity.entity.index].components.gameplayComponents,
                                       [&target](const auto &component) {
                return component.typeId == *target.component;
            });
        }

        /** @brief Resolves portable template interfaces only after complete reservation, before any owner preparation. */
        Result<void> ResolveInterfaces(const SceneCommandBuffer::CreateGroupCommand &group,
                                       const std::span<const EntityRef> entities) const {
            for (std::size_t index = 0; index < group.entries.size(); ++index) {
                const auto &entry = group.entries[index];
                if (!entry.members.empty() &&
                    entry.members.size() != entry.info.components.behaviors.size() + entry.info.components.gameplayComponents.size())
                    return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
                for (std::size_t member = 0; member < entry.members.size(); ++member) {
                    const auto &identity = entry.members[member];
                    const bool exists = identity.instance != 0 && std::visit([&]<typename Type>(const Type &type) {
                        if constexpr (std::is_same_v<Type, Gameplay::BehaviorTypeId>)
                            return std::ranges::any_of(entry.info.components.behaviors, [&](const auto &component) {
                                return component.typeId == type && component.instanceId.value == identity.instance;
                            });
                        else
                            return std::ranges::any_of(entry.info.components.gameplayComponents, [&](const auto &component) {
                                return component.typeId == type;
                            });
                    }, identity.type);
                    if (!exists || std::find(entry.members.begin(), entry.members.begin() + static_cast<std::ptrdiff_t>(member),
                                             identity) != entry.members.begin() + static_cast<std::ptrdiff_t>(member))
                        return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
                }
                std::vector<RuntimeResolvedGroupReference> resolved;
                resolved.reserve(entry.references.size());
                for (const auto &reference : entry.references) {
                    if (reference.ownerMember >= entry.members.size() || reference.property == 0 ||
                        std::ranges::any_of(resolved, [&](const auto &prior) {
                        return prior.owner == entry.members[reference.ownerMember] && prior.property == reference.property;
                    }))
                        return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
                    RuntimeResolvedGroupReference output{entry.members[reference.ownerMember], reference.property, {}};
                    const auto valid = std::visit([&]<typename Target>(const Target &target) {
                        if constexpr (std::is_same_v<Target, RuntimeGroupEntitySlot>) {
                            if (target.index >= entities.size())
                                return false;
                            output.target = entities[target.index];
                        } else if constexpr (std::is_same_v<Target, RuntimeGroupMemberSlot>) {
                            if (target.entity.index >= entities.size() ||
                                target.member >= group.entries[target.entity.index].members.size())
                                return false;
                            output.target = RuntimeGroupMemberReference{entities[target.entity.index],
                                                                        group.entries[target.entity.index].members[target.member]};
                        } else if constexpr (std::is_same_v<Target, Assets::AssetDependency>) {
                            if (!std::ranges::any_of(group.resources, [&target](const auto &resource) {
                                return resource.metadata.id == target.id && resource.metadata.expectedType == target.expectedType;
                            }))
                                return false;
                            output.target = target;
                        } else if constexpr (std::is_same_v<Target, RuntimeGroupExternalReference>) {
                            if (!scene.IsValid(scene.storage_, target.entity) || !HasExternal(target))
                                return false;
                            output.target = target;
                        }
                        return true;
                    }, reference.target);
                    if (!valid)
                        return Result<void>::Failure(MakeError(SceneErrors::StaleEntity));
                    resolved.push_back(std::move(output));
                }
                if (!resolved.empty())
                    candidate.slots[entities[index].entity.index].groupReferences =
                        std::make_shared<const std::vector<RuntimeResolvedGroupReference>>(std::move(resolved));
            }
            return Result<void>::Success();
        }

        /** @brief Validates one producer/target pair against candidate and resident Scene lifetimes. */
        Result<ResolvedGroupPhysicsBodyReference> ResolveGroupReference(
            const RuntimeComponentSet &components, const GroupPhysicsBodyReference &reference, const std::span<const EntityRef> entities,
            const std::span<const ResolvedGroupPhysicsBodyReference> resolved) const {
            if (const auto *source = SourceBodyReference(components, reference);
                !source || source->body != reference.body || !reference.body.IsValid() ||
                std::ranges::any_of(resolved, [&reference](const auto &other) {
                return other.kind == reference.kind && other.component == reference.component;
            }))
                return Result<ResolvedGroupPhysicsBodyReference>::Failure(
                    MakeError(SceneErrors::InvalidEntity, "Invalid or duplicated typed group body fixup."));
            EntityRef target;
            if (const auto *local = std::get_if<RuntimeGroupEntitySlot>(&reference.target)) {
                if (local->index >= entities.size())
                    return Result<ResolvedGroupPhysicsBodyReference>::Failure(MakeError(SceneErrors::InvalidEntity));
                target = entities[local->index];
            } else {
                target = std::get<EntityRef>(reference.target);
                if (!scene.IsValid(scene.storage_, target))
                    return Result<ResolvedGroupPhysicsBodyReference>::Failure(
                        MakeError(SceneErrors::StaleEntity, "External group targets must already belong to the committed Scene."));
            }
            if (!scene.IsValid(candidate, target))
                return Result<ResolvedGroupPhysicsBodyReference>::Failure(MakeError(SceneErrors::StaleEntity));
            if (const auto &body = candidate.slots[target.entity.index].components.rigidBody;
                !body || !body->enabled || body->body != reference.body)
                return Result<ResolvedGroupPhysicsBodyReference>::Failure(
                    MakeError(SceneErrors::InvalidEntity, "Group body target has no matching enabled slot."));
            return Result<ResolvedGroupPhysicsBodyReference>::Success({reference.kind, reference.component, target, reference.body});
        }

        /** @brief Resolves typed references only after every group entity has a candidate slot. */
        Result<void> ResolveGroupReferences(const SceneCommandBuffer::CreateGroupCommand &group,
                                            const std::span<const EntityRef> entities) const {
            for (std::size_t index = 0; index < group.entries.size(); ++index) {
                const auto &entry = group.entries[index];
                if (entry.physicsReferences.empty())
                    continue;
                auto resolved = std::make_shared<std::vector<ResolvedGroupPhysicsBodyReference>>();
                resolved->reserve(entry.physicsReferences.size());
                for (const auto &reference : entry.physicsReferences) {
                    auto fixup = ResolveGroupReference(entry.info.components, reference, entities, *resolved);
                    if (fixup.HasError())
                        return Result<void>::Failure(fixup.ErrorValue());
                    resolved->push_back(std::move(fixup).Value());
                }
                candidate.slots[entities[index].entity.index].groupPhysicsReferences = std::move(resolved);
            }
            return Result<void>::Success();
        }

        /** @brief Resolves the exact typed producer field without changing durable authoring metadata. */
        static const PhysicsBodyReference *SourceBodyReference(const RuntimeComponentSet &components,
                                                               const GroupPhysicsBodyReference &reference) noexcept {
            using enum GroupPhysicsReferenceKind;
            switch (reference.kind) {
                case ColliderBody:
                    return reference.component < components.colliders.size() ? &components.colliders[reference.component].body : nullptr;
                case ConstraintFirst:
                    return reference.component < components.physicsConstraints.size()
                               ? &components.physicsConstraints[reference.component].first.body
                               : nullptr;
                case ConstraintSecond:
                    if (reference.component < components.physicsConstraints.size()) {
                        if (const auto *body =
                                std::get_if<PhysicsConstraintBodyEndpoint>(&components.physicsConstraints[reference.component].second))
                            return &body->body;
                    }
                    return nullptr;
            }
            return nullptr;
        }

        /** @brief Validates final group body references before any native owner is prepared. */
        Result<void> ValidateGroupReferences() const {
            for (const auto &created : result.created) {
                const auto &slot = candidate.slots[created.entity.entity.index];
                if (!slot.active || !slot.groupReferences)
                    continue;
                for (const auto &reference : *slot.groupReferences) {
                    if (const auto *external = std::get_if<RuntimeGroupExternalReference>(&reference.target);
                        external && !HasExternal(*external))
                        return Result<void>::Failure(MakeError(SceneErrors::StaleEntity));
                }
            }
            // A later command may retire a body after a group's fixups were resolved. Validate the
            // final candidate before preparing any native owner; inactive source entities need no bindings.
            for (const auto &slot : candidate.slots) {
                if (!slot.active || !slot.groupPhysicsReferences)
                    continue;
                for (const auto &reference : *slot.groupPhysicsReferences) {
                    if (!scene.IsValid(candidate, reference.target))
                        return Result<void>::Failure(
                            MakeError(SceneErrors::StaleEntity, "Group body target does not survive the complete command buffer."));
                    const auto &body = candidate.slots[reference.target.entity.index].components.rigidBody;
                    if (!body || !body->enabled || body->body != reference.body)
                        return Result<void>::Failure(
                            MakeError(SceneErrors::InvalidEntity, "Final group body target has no matching enabled slot."));
                }
            }
            return Result<void>::Success();
        }

        /** @brief Projects additions and retirements from detached storage, retaining every admitted asset lease. */
        bool ProjectResourceGroup(std::vector<RuntimeEntityView> &created, std::vector<EntityRef> &destroyed) const {
            bool resourceGroup{};
            created.reserve(result.created.size());
            for (const auto &resolution : result.created) {
                if (!scene.IsValid(candidate, resolution.entity))
                    continue;
                const auto &slot = candidate.slots[resolution.entity.entity.index];
                if (!slot.groupResources)
                    continue;
                resourceGroup = true;
                const auto parent = slot.parent ? std::optional<EntityRef>{{scene.runtimeId_, *slot.parent}} : std::nullopt;
                created.emplace_back(resolution.entity, slot.authoredObject, parent, &slot.localTransform, &slot.primitiveMesh,
                                     &slot.components,
                                     slot.groupPhysicsReferences
                                         ? std::span<const ResolvedGroupPhysicsBodyReference>{*slot.groupPhysicsReferences}
                                         : std::span<const ResolvedGroupPhysicsBodyReference>{},
                                     std::span<const RuntimeGroupAssetLease>{*slot.groupResources},
                                     slot.groupReferences ? std::span<const RuntimeResolvedGroupReference>{*slot.groupReferences}
                                                          : std::span<const RuntimeResolvedGroupReference>{},
                                     slot.groupSpawnLineage ? std::span<const Assets::AssetId>{*slot.groupSpawnLineage}
                                                            : std::span<const Assets::AssetId>{});
            }
            for (std::size_t index = 0; index < scene.storage_.slots.size(); ++index) {
                const auto &old = scene.storage_.slots[index];
                if (old.active && old.groupResources &&
                    (!candidate.slots[index].active || candidate.slots[index].generation != old.generation)) {
                    resourceGroup = true;
                    destroyed.emplace_back(scene.runtimeId_, EntityId{static_cast<std::uint32_t>(index), old.generation});
                }
            }
            return resourceGroup;
        }

        /** @brief Requires explicit composition of each owner needed by the projected typed components. */
        Result<void> ValidateOwners(const std::span<const RuntimeEntityView> created, const std::span<const EntityRef> destroyed,
                                    const std::span<const std::unique_ptr<SceneStructuralParticipant>> participants) const {
            const auto needs = [&](SceneStructuralOwner owner) {
                const auto componentsNeed = [owner](const RuntimeComponentSet &components) {
                    using enum SceneStructuralOwner;
                    switch (owner) {
                        case Physics:
                            return components.rigidBody.has_value() || !components.colliders.empty() ||
                                   !components.physicsConstraints.empty();
                        case Gameplay:
                            return !components.behaviors.empty() || !components.gameplayComponents.empty();
                        case AI:
                            return components.aiAgent.has_value() || components.aiController.has_value();
                    }
                    return false;
                };
                return std::ranges::any_of(created, [&](const auto &entity) {
                    return componentsNeed(*entity.components);
                }) || std::ranges::any_of(destroyed, [&](const auto &entity) {
                    return componentsNeed(scene.storage_.slots[entity.entity.index].components);
                });
            };
            for (const auto owner : {SceneStructuralOwner::Physics, SceneStructuralOwner::Gameplay, SceneStructuralOwner::AI}) {
                if (needs(owner) && !std::ranges::any_of(participants, [owner](const auto &participant) {
                    return participant->Owner() == owner;
                }))
                    return Result<void>::Failure(
                        MakeError(SceneErrors::AssetServicesUnavailable, "Required structural subsystem owner is not composed."));
            }
            return Result<void>::Success();
        }

        /** @brief Prepares and validates all owner candidates before Scene's final admission fence. */
        Result<void> PrepareOwners(const std::span<const std::unique_ptr<SceneStructuralParticipant>> participants,
                                   std::vector<std::unique_ptr<SceneStructuralCandidate>> &owners) const {
            if (const auto valid = ValidateGroupReferences(); valid.HasError())
                return valid;
            std::vector<RuntimeEntityView> created;
            std::vector<EntityRef> destroyed;
            const bool resourceGroup = ProjectResourceGroup(created, destroyed);
            if (const auto valid = ValidateOwners(created, destroyed, participants); valid.HasError())
                return valid;
            owners.reserve(participants.size());
            if (resourceGroup) {
                for (const auto &participant : participants) {
                    auto prepared = participant->Prepare(scene.View(), created, destroyed);
                    if (prepared.HasError())
                        return Result<void>::Failure(prepared.ErrorValue());
                    if (!prepared.Value())
                        return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate));
                    owners.push_back(std::move(prepared).Value());
                }
                for (const auto &owner : owners) {
                    if (auto valid = owner->ValidatePublication(); valid.HasError())
                        return Result<void>::Failure(valid.ErrorValue());
                }
            }
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
