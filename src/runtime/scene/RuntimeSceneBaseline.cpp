#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>

namespace Horo::Runtime {
    namespace {
        Result<void> ValidateBaselineCapacity(const RuntimeSceneStorage &storage, const RuntimeSceneDefinition &definition,
                                              const std::size_t resourceCount, const SceneBaselineAttachmentLimits limits) {
            if (storage.baselines.size() >= limits.maximumAttachments)
                return Result<void>::Failure(MakeError(SceneErrors::BaselineCapacityExceeded));
            std::size_t entities = definition.Entities().size();
            std::size_t resources = resourceCount;
            for (const auto &baseline : storage.baselines) {
                if (baseline.entities.size() > limits.maximumEntities - entities ||
                    baseline.resources->size() > limits.maximumResources - resources)
                    return Result<void>::Failure(MakeError(SceneErrors::BaselineCapacityExceeded));
                entities += baseline.entities.size();
                resources += baseline.resources->size();
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RuntimeScene::RemoveBaseline */
    Result<void> RuntimeScene::RemoveBaseline(RuntimeSceneStorage &storage, const SceneDefinitionId id,
                                              const SceneDefinitionRevision revision, const SceneBaselineOwnership &ownership) const {
        const auto found = std::ranges::find(storage.baselines, id, &BaselineAttachment::id);
        if (found == storage.baselines.end() || found->revision != revision || !found->ownership->Matches(ownership))
            return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
        auto remaining = found->entities;
        while (!remaining.empty()) {
            const auto leaf = std::ranges::find_if(remaining, [&](const EntityId entity) {
                return std::ranges::none_of(storage.slots, [&](const Slot &slot) {
                    return slot.active && slot.parent == entity;
                });
            });
            if (leaf == remaining.end())
                return Result<void>::Failure(
                    MakeError(SceneErrors::BaselineInvalid, "A resident outside this baseline still depends on its hierarchy."));
            if (const auto removed = DestroyEntity(storage, {runtimeId_, *leaf}); removed.HasError())
                return removed;
            remaining.erase(leaf);
        }
        storage.baselines.erase(found);
        return Result<void>::Success();
    }

    /** @copydoc RuntimeScene::ApplyBaseline */
    Result<void> RuntimeScene::ApplyBaseline(RuntimeSceneStorage &storage, const SceneCommandBuffer::AttachBaselineCommand &command) const {
        const auto &definition = command.definition;
        const auto found = std::ranges::find(storage.baselines, definition.Id(), &BaselineAttachment::id);
        if (definition.Id() == definitionId_ || (command.expectedRevision.value == 0 && found != storage.baselines.end()))
            return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
        if (command.expectedRevision.value != 0) {
            if (definition.Revision().value <= command.expectedRevision.value)
                return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
            if (const auto removed = RemoveBaseline(storage, definition.Id(), command.expectedRevision, *command.ownership);
                removed.HasError())
                return removed;
        }
        const auto limits = command.limits;
        if (const auto capacity = ValidateBaselineCapacity(storage, definition, command.resources.size(), limits); capacity.HasError())
            return capacity;
        BaselineAttachment baseline{definition.Id(),
                                    definition.Revision(),
                                    {},
                                    std::make_shared<const std::vector<RuntimeGroupAssetLease>>(command.resources),
                                    command.ownership};
        baseline.entities.reserve(definition.Entities().size());
        for (const auto &source : definition.Entities()) {
            auto created = CreateEntity(storage, {source.localTransform, {}, source.object, source.primitiveMesh, source.components});
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            baseline.entities.push_back(created.Value().entity);
            auto &slot = storage.slots[created.Value().entity.index];
            slot.baselineOwner = definition.Id();
            slot.groupResources = baseline.resources;
        }
        for (std::size_t index = 0; index < definition.Entities().size(); ++index) {
            const auto &source = definition.Entities()[index];
            if (!source.parent)
                continue;
            const auto parent = std::ranges::find(definition.Entities(), *source.parent, &RuntimeEntityDefinition::object);
            if (parent == definition.Entities().end())
                return Result<void>::Failure(MakeError(SceneErrors::ParentNotFound));
            storage.slots[baseline.entities[index].index].parent =
                baseline.entities[static_cast<std::size_t>(parent - definition.Entities().begin())];
        }
        if (const auto bound = BindBaselineReferences(storage, baseline); bound.HasError())
            return bound;
        storage.baselines.push_back(std::move(baseline));
        return Result<void>::Success();
    }

    /** @copydoc RuntimeScene::BindBaselineReferences */
    Result<void> RuntimeScene::BindBaselineReferences(RuntimeSceneStorage &storage, const BaselineAttachment &baseline) const {
        for (const auto entity : baseline.entities) {
            auto &slot = storage.slots[entity.index];
            std::vector<ResolvedGroupPhysicsBodyReference> references;
            const auto bind = [&](const GroupPhysicsReferenceKind kind, const std::size_t occurrence,
                                  const PhysicsBodyReference source) -> Result<void> {
                const auto target = std::ranges::find_if(baseline.entities, [&](const EntityId candidate) {
                    return storage.slots[candidate.index].authoredObject == source.object;
                });
                if (target == baseline.entities.end())
                    return Result<void>::Failure(MakeError(SceneErrors::BaselineInvalid));
                const auto &body = storage.slots[target->index].components.rigidBody;
                if (!body || !body->enabled || body->body != source.body)
                    return Result<void>::Failure(MakeError(SceneErrors::BaselineInvalid));
                references.push_back({kind, occurrence, {runtimeId_, *target}, source.body});
                return Result<void>::Success();
            };
            for (std::size_t index = 0; index < slot.components.colliders.size(); ++index) {
                if (const auto valid = bind(GroupPhysicsReferenceKind::ColliderBody, index, slot.components.colliders[index].body);
                    valid.HasError())
                    return valid;
            }
            for (std::size_t index = 0; index < slot.components.physicsConstraints.size(); ++index) {
                const auto &constraint = slot.components.physicsConstraints[index];
                if (const auto valid = bind(GroupPhysicsReferenceKind::ConstraintFirst, index, constraint.first.body); valid.HasError())
                    return valid;
                if (const auto *second = std::get_if<PhysicsConstraintBodyEndpoint>(&constraint.second)) {
                    if (const auto valid = bind(GroupPhysicsReferenceKind::ConstraintSecond, index, second->body); valid.HasError())
                        return valid;
                }
            }
            if (!references.empty())
                slot.groupPhysicsReferences = std::make_shared<const std::vector<ResolvedGroupPhysicsBodyReference>>(std::move(references));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
