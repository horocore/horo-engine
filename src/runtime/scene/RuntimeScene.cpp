#include "Horo/Runtime/Scene/RuntimeScene.h"

#include "Horo/Assets/AssetProvider.h"
#include "RuntimeSceneCommandApplier.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <new>
#include <ranges>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace Horo::Runtime {
    /** @copydoc SceneActivationParticipant::PrepareRetirement */
    Result<std::unique_ptr<SceneActivationCandidate>> SceneActivationParticipant::PrepareRetirement(const RuntimeSceneView &scene) {
        (void)scene;
        return Result<std::unique_ptr<SceneActivationCandidate>>::Success({});
    }

    /** @copydoc RuntimeScene::~RuntimeScene */
    RuntimeScene::~RuntimeScene() {
        // Revoke borrows before any owned storage or subsystem teardown can release/reuse this address.
        viewLifetime_.reset();
    }

    /** @copydoc RuntimeScene::AttachVfxWorld */
    Result<void> RuntimeScene::AttachVfxWorld(std::unique_ptr<Vfx::VfxWorld> world, const Math::WorldCoordinate64 &origin,
                                              std::uint64_t originRevision) {
        if (!world || vfxWorld_ || world->Scene().Value() != runtimeId_.value || originRevision == 0)
            return Result<void>::Failure(MakeError(Vfx::VfxErrors::IdentityInvalid));
        vfxWorld_ = std::move(world);
        vfxOrigin_ = origin;
        vfxOriginRevision_ = originRevision;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeScene::CaptureVfx */
    Vfx::VfxExtractionOutcome RuntimeScene::CaptureVfx(std::uint64_t frame) noexcept {
        return vfxWorld_ ? vfxWorld_->Extract(frame, vfxOrigin_, vfxOriginRevision_) : Vfx::VfxExtractionOutcome{};
    }

    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &code, std::string message) {
            return Result<T>::Failure(MakeError(code, std::move(message)));
        }

        /** @brief Rechecks shared structural/owner cancellation and receiving Scene identity. */
        Result<void> ValidateSceneAdmission(const SceneStructuralAdmission &admission, SceneRuntimeId scene,
                                            std::optional<Assets::AssetRegistryRevision> registry = {}) {
            if (admission.cancellation.IsCancellationRequested() || admission.ownerCancellation.IsCancellationRequested() ||
                admission.scopeCancellation.IsCancellationRequested())
                return JobCancelled();
            if (admission.scene != scene)
                return Result<void>::Failure(MakeError(SceneErrors::StaleEntity));
            if (registry.has_value() && admission.registry != *registry)
                return Result<void>::Failure(MakeError(SceneErrors::AssetRevisionStale));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SceneCommandBuffer::Create */
    DeferredEntity SceneCommandBuffer::Create(RuntimeEntityCreateInfo createInfo) {
        const DeferredEntity deferred{nextDeferred_++};
        commands_.emplace_back(CreateCommand{deferred, std::move(createInfo)});
        return deferred;
    }

    namespace {
        /** @brief Overflow-safe whole-group admission shared by ordinary and private service-selected policies. */
        bool AdmitsGroupBounds(std::size_t entities, std::size_t resources, const SceneStructuralAdmission &admission,
                               const SceneRestoreStructuralLimits &limits, std::uint64_t nextDeferred) noexcept {
            return entities != 0 && entities <= limits.maximumEntities && resources <= limits.maximumResources &&
                   admission.scene.IsValid() && admission.registry.value != 0 &&
                   entities <= std::numeric_limits<std::uint64_t>::max() - nextDeferred;
        }

        /** @brief Exact original per-entity lineage checks, without inheriting ancestry from neighboring restore occurrences. */
        Result<void> ValidateGroupLineage(std::span<const Assets::AssetId> lineage) {
            if (lineage.size() > 16)
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            for (std::size_t index = 0; index < lineage.size(); ++index)
                if (!lineage[index].IsValid() || std::find(lineage.begin(), lineage.begin() + static_cast<std::ptrdiff_t>(index),
                                                           lineage[index]) != lineage.begin() + static_cast<std::ptrdiff_t>(index))
                    return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            return Result<void>::Success();
        }

        /** @brief One parent-before-child entry and exact bounded interface/lineage shape. */
        Result<void> ValidateGroupEntry(std::span<const RuntimeEntityGroupEntry> entries, std::size_t index, bool uniformLineage,
                                        std::size_t partitionCount) {
            const auto &entry = entries[index];
            if (entry.parentInGroup.has_value() && (*entry.parentInGroup >= index || entry.info.parent))
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity, "Group parent must precede its child."));
            if (entry.members.size() > 64 || (uniformLineage && entry.spawnLineage != entries.front().spawnLineage))
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            const bool partitionValid = uniformLineage ? !entry.restorePartition.has_value()
                                                       : entry.restorePartition.has_value() && *entry.restorePartition < partitionCount;
            if (!partitionValid)
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            return ValidateGroupLineage(entry.spawnLineage);
        }

        /** @brief Shared whole-batch accounting; private restore admission cannot bypass lower service-clamped budgets. */
        Result<void> ValidateGroupEntries(std::span<const RuntimeEntityGroupEntry> entries, const SceneRestoreStructuralLimits &limits,
                                          bool uniformLineage, std::size_t partitionCount) {
            std::size_t physics{};
            std::size_t references{};
            std::size_t lineage{};
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto &entry = entries[index];
                if (auto valid = ValidateGroupEntry(entries, index, uniformLineage, partitionCount); valid.HasError())
                    return valid;
                if (entry.physicsReferences.size() > limits.maximumPhysicsReferences - physics ||
                    entry.references.size() > limits.maximumReferences - references ||
                    entry.spawnLineage.size() > limits.maximumLineageEntries - lineage)
                    return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
                physics += entry.physicsReferences.size();
                references += entry.references.size();
                lineage += entry.spawnLineage.size();
            }
            return Result<void>::Success();
        }

        /** @brief Validates one declared dense member against actual prepared entries, independently of durable record admission. */
        Result<void> ValidatePartitionSlot(std::span<const RuntimeEntityGroupEntry> entries, const SceneRestorePartitionMember &member,
                                           std::size_t partition, std::size_t root, std::vector<bool> &seen) {
            if (member.entity >= entries.size() || seen[member.entity] || entries[member.entity].restorePartition != partition)
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            seen[member.entity] = true;
            if (member.entity == root)
                return Result<void>::Success();
            const auto parent = entries[member.entity].parentInGroup;
            if (!parent || entries[*parent].restorePartition != partition)
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            return Result<void>::Success();
        }

        /** @brief A partition's actual root entry must belong to that exact private partition. */
        bool ValidPartitionRoot(std::span<const RuntimeEntityGroupEntry> entries, const SceneRestoreGroupPartition &partition,
                                std::size_t index) noexcept {
            return !partition.members.empty() && partition.root < entries.size() && entries[partition.root].restorePartition == index;
        }

        /** @brief Requires complete unambiguous partition membership before any Scene-owned pins/entities are reserved. */
        Result<void> ValidateGroupPartitions(std::span<const RuntimeEntityGroupEntry> entries,
                                             std::span<const SceneRestoreGroupPartition> partitions, bool uniformLineage) {
            if (uniformLineage)
                return partitions.empty() ? Result<void>::Success() : Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            std::vector<bool> seen(entries.size());
            for (std::size_t index = 0; index < partitions.size(); ++index) {
                const auto &partition = partitions[index];
                if (!ValidPartitionRoot(entries, partition, index))
                    return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
                for (const auto &member : partition.members)
                    if (auto valid = ValidatePartitionSlot(entries, member, index, partition.root, seen); valid.HasError())
                        return valid;
                if (!seen[partition.root])
                    return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            }
            return std::ranges::all_of(seen,
                                       [](bool value) {
                return value;
            })
                       ? Result<void>::Success()
                       : Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
        }

        /** @brief Same named-resource validation plus explicit aggregate resident-byte admission for private restore. */
        Result<void> ValidateGroupResources(std::span<const RuntimeGroupAssetLease> resources, std::size_t maximumBytes) {
            std::size_t bytes{};
            for (const auto &pin : resources) {
                if (pin.artifact.Bytes().empty() || !pin.metadata.id.IsValid() || pin.metadata.expectedType.Value().empty())
                    return Result<void>::Failure(MakeError(SceneErrors::AssetPayloadEmpty));
                if (pin.artifact.Bytes().size() > maximumBytes - bytes)
                    return Result<void>::Failure(
                        MakeError(SceneErrors::StructuralCommitFailed, "Restore group exceeds actual host resource-byte budget."));
                bytes += pin.artifact.Bytes().size();
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SceneAggregateRestore::ValidateRestorePartitions */
    Result<void> SceneAggregateRestore::ValidateRestorePartitions(std::span<const SceneRestoreGroupPartition> partitions) const {
        return partitions.empty() ? Result<void>::Success() : Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate));
    }

    /** @copydoc SceneCommandBuffer::CreateGroup */
    Result<std::vector<DeferredEntity>> SceneCommandBuffer::CreateGroup(std::vector<RuntimeEntityGroupEntry> entries,
                                                                        std::vector<RuntimeGroupAssetLease> resources,
                                                                        const SceneStructuralAdmission &admission) {
        return CreateGroupInternal(std::move(entries), std::move(resources), admission, {}, {});
    }

    /** @copydoc SceneCommandBuffer::CreateGroupInternal */
    Result<std::vector<DeferredEntity>> SceneCommandBuffer::CreateGroupInternal(std::vector<RuntimeEntityGroupEntry> entries,
                                                                                std::vector<RuntimeGroupAssetLease> resources,
                                                                                const SceneStructuralAdmission &admission,
                                                                                const GroupPolicy &policy,
                                                                                std::vector<SceneRestoreGroupPartition> partitions) {
        const auto &limits = policy.limits;
        if (!AdmitsGroupBounds(entries.size(), resources.size(), admission, limits, nextDeferred_))
            return Result<std::vector<DeferredEntity>>::Failure(
                MakeError(SceneErrors::StructuralCommitFailed, "Invalid structural group bounds."));
        if (auto valid = ValidateGroupEntries(entries, limits, policy.uniformLineage, partitions.size()); valid.HasError())
            return Result<std::vector<DeferredEntity>>::Failure(valid.ErrorValue());
        if (auto valid = ValidateGroupPartitions(entries, partitions, policy.uniformLineage); valid.HasError())
            return Result<std::vector<DeferredEntity>>::Failure(valid.ErrorValue());
        if (auto valid = ValidateGroupResources(resources, limits.maximumResourceBytes); valid.HasError())
            return Result<std::vector<DeferredEntity>>::Failure(valid.ErrorValue());
        std::vector<DeferredEntity> tokens;
        tokens.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index)
            tokens.emplace_back(nextDeferred_ + index);
        auto returned = tokens;
        commands_.emplace_back(CreateGroupCommand{std::move(entries), std::move(resources), std::move(tokens), admission,
                                                  policy.uniformLineage, std::move(partitions)});
        nextDeferred_ += returned.size();
        return Result<std::vector<DeferredEntity>>::Success(std::move(returned));
    }

    /** @copydoc SceneCommandBuffer::HasRestoreGroups */
    bool SceneCommandBuffer::HasRestoreGroups() const noexcept {
        return std::ranges::any_of(commands_, [](const auto &command) {
            const auto *group = std::get_if<CreateGroupCommand>(&command);
            return group && group->restoreSeal;
        });
    }

    /** @copydoc SceneCommandBuffer::ValidateAdmission */
    Result<void> SceneCommandBuffer::ValidateAdmission(SceneRuntimeId scene, Assets::AssetRegistryRevision registry) const {
        for (const auto &command : commands_) {
            const auto valid = std::visit([&]<typename T>(const T &value) -> Result<void> {
                if constexpr (std::is_same_v<T, SetLocalTransformCommand>) {
                    return value.admission ? ValidateSceneAdmission(*value.admission, scene) : Result<void>::Success();
                } else if constexpr (std::is_same_v<T, CreateGroupCommand> || std::is_same_v<T, DestroyGroupCommand> ||
                                     std::is_same_v<T, AttachBaselineCommand> || std::is_same_v<T, DetachBaselineCommand>) {
                    constexpr bool checksRegistry = !std::is_same_v<T, DestroyGroupCommand>;
                    if (const auto admitted =
                            ValidateSceneAdmission(value.admission, scene, checksRegistry ? std::optional{registry} : std::nullopt);
                        admitted.HasError())
                        return admitted;
                    if constexpr (std::is_same_v<T, AttachBaselineCommand> || std::is_same_v<T, DetachBaselineCommand>)
                        return value.publicationCheck->ValidatePublication();
                }
                return Result<void>::Success();
            }, command);
            if (valid.HasError())
                return valid;
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
        commands_.emplace_back(SetLocalTransformCommand{entity, std::move(localTransform), std::nullopt});
    }

    /** @copydoc SceneCommandBuffer::SetLocalTransform */
    void SceneCommandBuffer::SetLocalTransform(const EntityRef entity, Math::Transform localTransform,
                                               const SceneStructuralAdmission &admission) {
        commands_.emplace_back(SetLocalTransformCommand{entity, std::move(localTransform), admission});
    }

    /** @copydoc SceneCommandBuffer::Empty */
    bool SceneCommandBuffer::Empty() const noexcept {
        return commands_.empty();
    }

    /** @copydoc RuntimeScene::RuntimeScene */
    RuntimeScene::RuntimeScene(const SceneRuntimeId runtimeId, const SceneDefinitionId definitionId, const SceneDefinitionRevision revision,
                               const RuntimeSceneConfig config, const Assets::AssetRegistryRevision assetRevision,
                               std::vector<ResolvedAsset> &&assets)
        : viewLifetime_(std::make_shared<const std::uint8_t>(0)), runtimeId_(runtimeId), definitionId_(definitionId),
          definitionRevision_(revision), assetRegistryRevision_(assetRevision), assets_(std::size_t{0}), config_(config) {
        assets_.swap(assets);
    }

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
                                                                       std::vector<ResolvedAsset> &&assets) {
        if (!runtimeId.IsValid() || config.maximumGeneration == 0)
            return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::InvalidCandidate,
                                                          "Runtime identity and maximum generation must be non-zero.");
        if (assets.size() != definition.AssetDependencies().size())
            return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::InvalidCandidate,
                                                          "Resolved asset set does not match the definition.");
        std::unique_ptr<RuntimeScene> scene;
        try {
            scene =
                std::make_unique<RuntimeScene>(runtimeId, definition.Id(), definition.Revision(), config, assetRevision, std::move(assets));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::InvalidCandidate,
                                                          "Unable to allocate Scene owner lifetime evidence.");
        }
        scene->storage_.slots.reserve(definition.Entities().size());
        scene->storage_.authoredIndex.reserve(definition.Entities().size());

#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL != 0
        std::optional<std::map<std::uint64_t, EntityId>> parentIndex;
#else
        std::optional<std::unordered_map<std::uint64_t, EntityId>> parentIndex;
#endif
        if (std::ranges::any_of(definition.Entities(), [](const auto &entity) {
            return entity.parent.has_value();
        })) {
            parentIndex.emplace();
#if !defined(_MSVC_STL_VERSION) || _ITERATOR_DEBUG_LEVEL == 0
            parentIndex->reserve(definition.Entities().size());
#endif
        }
        for (const RuntimeEntityDefinition &entity : definition.Entities()) {
            RuntimeEntityCreateInfo info{entity.localTransform, std::nullopt, entity.object, entity.primitiveMesh, entity.components};
            // Definition admission proves schema and unique IDs; this candidate contains only entries from that definition.
            Result<EntityRef> created = scene->InsertValidatedEntity(scene->storage_, info);
            if (created.HasError())
                return Result<std::unique_ptr<RuntimeScene>>::Failure(created.ErrorValue());
            if (parentIndex)
                parentIndex->emplace(entity.object.value, created.Value().entity);
        }
        for (const RuntimeEntityDefinition &entity : definition.Entities()) {
            if (!entity.parent)
                continue;
            const auto child = parentIndex->find(entity.object.value);
            const auto parent = parentIndex->find(entity.parent->value);
            if (child == parentIndex->end() || parent == parentIndex->end())
                return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::ParentNotFound,
                                                              "Definition parent resolution failed during instantiation.");
            scene->storage_.slots[child->second.index].parent = parent->second;
        }
        return Result<std::unique_ptr<RuntimeScene>>::Success(std::move(scene));
    }

    /** @copydoc RuntimeSceneService::CloneActive */
    Result<std::unique_ptr<RuntimeScene>> RuntimeSceneService::CloneActive(const SceneRuntimeId runtimeId) const {
        if (!active_.scene || !runtimeId.IsValid() || runtimeId == active_.scene->runtimeId_)
            return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::InvalidCandidate,
                                                          "An active scene and a distinct runtime identity are required.");
        if (active_.scene->vfxWorld_)
            return Result<std::unique_ptr<RuntimeScene>>::Failure(MakeError(Vfx::VfxErrors::SceneCloneUnavailable));
        std::unique_ptr<RuntimeScene> clone;
        try {
            auto assets = active_.scene->assets_;
            clone = std::make_unique<RuntimeScene>(runtimeId, active_.scene->definitionId_, active_.scene->definitionRevision_,
                                                   active_.scene->config_, active_.scene->assetRegistryRevision_, std::move(assets));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<RuntimeScene>>(SceneErrors::InvalidCandidate,
                                                          "Unable to allocate cloned Scene lifetime evidence.");
        }
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
        return ownerThread_ == std::this_thread::get_id() ? RuntimeSceneView{*this} : RuntimeSceneView{};
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

        return InsertValidatedEntity(storage, info);
    }

    /** @copydoc RuntimeScene::InsertValidatedEntity */
    Result<EntityRef> RuntimeScene::InsertValidatedEntity(RuntimeSceneStorage &storage, const RuntimeEntityCreateInfo &info) const {
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
        slot.baselineOwner.reset();
        slot.groupResources.reset();
        slot.groupIdentity.reset();
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

    /** @copydoc SceneStructuralParticipant::ValidateBaselines */
    Result<void> SceneStructuralParticipant::ValidateBaselines(const RuntimeSceneView &,
                                                               const std::span<const SceneBaselinePreparationView> baselines) const {
        if (Owner() == SceneStructuralOwner::Physics && std::ranges::any_of(baselines, [](const auto &baseline) {
            return !baseline.definition.TriggerVolumeEvidence().empty();
        }))
            return Result<void>::Failure(
                MakeError(SceneErrors::BaselineUnsupported,
                          "The composed Physics structural owner does not validate required trigger migration proof."));
        return Result<void>::Success();
    }

    /** @copydoc RuntimeScene::Commit */
    Result<StructuralCommitResult> RuntimeScene::Commit(const SceneCommandBuffer &commands) {
        return CommitWithRegistry(commands, nullptr);
    }

    /** @copydoc RuntimeScene::CommitWithRegistry */
    Result<StructuralCommitResult> RuntimeScene::CommitWithRegistry(
        const SceneCommandBuffer &commands, const Assets::AssetRegistry *registry,
        std::span<const std::unique_ptr<SceneStructuralParticipant>> participants, std::optional<Error> *notificationError,
        CommitPurpose purpose) {
        if (commands.Empty())
            return Result<StructuralCommitResult>::Success({});
        if (auto admission = CommandApplier::ValidateResourceAdmission(commands, runtimeId_, registry, purpose); admission.HasError())
            return Result<StructuralCommitResult>::Failure(admission.ErrorValue());
        if (auto valid = CommandApplier::ValidateBaselineParticipants(commands, View(), participants); valid.HasError())
            return Result<StructuralCommitResult>::Failure(valid.ErrorValue());
        RuntimeSceneStorage candidate = storage_;
        StructuralCommitResult result;
        result.created.reserve(commands.commands_.size());
        const CommandApplier applier{*this, candidate, result};
        if (const auto status = applier.ApplyCommands(commands); status.HasError())
            return Result<StructuralCommitResult>::Failure(status.ErrorValue());
        std::vector<std::unique_ptr<SceneStructuralCandidate>> owners;
        if (const auto ready = applier.PrepareOwners(participants, owners, purpose); ready.HasError())
            return Result<StructuralCommitResult>::Failure(ready.ErrorValue());
        // Candidate preparation may allocate. Cancellation is sampled again only after all fallible work,
        // immediately before publication. Registry mutation is restricted to this same owner lane.
        const auto finalRevision = registry ? registry->Snapshot().Revision() : Assets::AssetRegistryRevision{};
        if (auto admission = commands.ValidateAdmission(runtimeId_, finalRevision); admission.HasError())
            return Result<StructuralCommitResult>::Failure(admission.ErrorValue());
        for (const auto &owner : owners)
            owner->Publish();
        RuntimeSceneStorage retired = std::move(storage_);
        storage_ = std::move(candidate);
        ++structuralRevision_;
        CommandApplier::NotifyOwners(owners, notificationError);
        return Result<StructuralCommitResult>::Success(std::move(result));
    }

}  // namespace Horo::Runtime
