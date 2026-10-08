#pragma once

/** @file RuntimeSceneCommandApplier.h @brief Target-private structural command application against detached Scene storage. */
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace Horo::Runtime {
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
            if (scene.IsValid(candidate, destroy.entity) && candidate.slots[destroy.entity.entity.index].baselineOwner)
                return Result<void>::Failure(
                    MakeError(SceneErrors::BaselineInvalid, "Baseline entities retire only through exact baseline ownership."));
            if (const Result<void> destroyedResult = scene.DestroyEntity(candidate, destroy.entity); destroyedResult.HasError())
                return Result<void>::Failure(destroyedResult.ErrorValue());
            ++result.destroyed;
            return Result<void>::Success();
        }

        Result<void> operator()(const SceneCommandBuffer::AttachBaselineCommand &command) const {
            auto attached = scene.ApplyBaseline(candidate, command);
            if (attached.HasValue())
                ++result.baselinesAttached;
            return attached;
        }

        Result<void> operator()(const SceneCommandBuffer::DetachBaselineCommand &command) const {
            auto detached = scene.RemoveBaseline(candidate, command.id, command.revision, *command.ownership);
            if (detached.HasValue())
                ++result.baselinesDetached;
            return detached;
        }

        Result<void> operator()(const SceneCommandBuffer::DestroyGroupCommand &group) const {
            const auto root = group.entities.front();
            if (!scene.IsValid(candidate, root))
                return Result<void>::Failure(MakeError(SceneErrors::StaleEntity));
            if (candidate.slots[root.entity.index].baselineOwner)
                return Result<void>::Failure(
                    MakeError(SceneErrors::BaselineInvalid, "Baseline entities retire only through exact baseline ownership."));
            const auto resources = candidate.slots[root.entity.index].groupResources;
            if (!resources)
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            if (const auto count = std::ranges::count_if(candidate.slots,
                                                         [&resources](const auto &slot) {
                return slot.active && slot.groupResources == resources;
            });
                static_cast<std::size_t>(count) != group.entities.size())
                return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            for (const auto entity : group.entities) {
                if (!scene.IsValid(candidate, entity) || candidate.slots[entity.entity.index].groupResources != resources)
                    return Result<void>::Failure(MakeError(SceneErrors::StaleEntity));
                if (candidate.slots[entity.entity.index].baselineOwner)
                    return Result<void>::Failure(
                        MakeError(SceneErrors::BaselineInvalid, "Baseline entities retire only through exact baseline ownership."));
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

        /** @brief Validates complete occurrence metadata before resolving its interfaces. */
        [[nodiscard]] static Result<void> ValidateInterfaceMembers(const RuntimeEntityGroupEntry &entry) {
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
                if (!exists || std::find(entry.members.begin(), entry.members.begin() + static_cast<std::ptrdiff_t>(member), identity) !=
                                   entry.members.begin() + static_cast<std::ptrdiff_t>(member))
                    return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
            }
            return Result<void>::Success();
        }

        /** @brief Resolves one local entity slot only against the fully reserved group. */
        static bool ResolveInterfaceEntity(const RuntimeGroupEntitySlot target, const std::span<const EntityRef> entities,
                                           RuntimeResolvedGroupReference &output) {
            if (target.index >= entities.size())
                return false;
            output.target = entities[target.index];
            return true;
        }

        /** @brief Resolves one local member against both entity and occurrence bounds. */
        static bool ResolveInterfaceMember(const RuntimeGroupMemberSlot target, const SceneCommandBuffer::CreateGroupCommand &group,
                                           const std::span<const EntityRef> entities, RuntimeResolvedGroupReference &output) {
            if (target.entity.index >= entities.size() || target.member >= group.entries[target.entity.index].members.size())
                return false;
            output.target =
                RuntimeGroupMemberReference{entities[target.entity.index], group.entries[target.entity.index].members[target.member]};
            return true;
        }

        /** @brief Validates one portable interface target against queued resources and live external authority. */
        bool ResolveInterfaceTarget(const RuntimeGroupReference &reference, const SceneCommandBuffer::CreateGroupCommand &group,
                                    const std::span<const EntityRef> entities, RuntimeResolvedGroupReference &output) const {
            return std::visit([this, &group, entities, &output]<typename Target>(const Target &target) {
                if constexpr (std::is_same_v<Target, RuntimeGroupEntitySlot>)
                    return ResolveInterfaceEntity(target, entities, output);
                else if constexpr (std::is_same_v<Target, RuntimeGroupMemberSlot>)
                    return ResolveInterfaceMember(target, group, entities, output);
                else if constexpr (std::is_same_v<Target, Assets::AssetDependency>) {
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
        }

        /** @brief Resolves portable template interfaces only after complete reservation, before any owner preparation. */
        Result<void> ResolveInterfaces(const SceneCommandBuffer::CreateGroupCommand &group,
                                       const std::span<const EntityRef> entities) const {
            for (std::size_t index = 0; index < group.entries.size(); ++index) {
                const auto &entry = group.entries[index];
                if (auto valid = ValidateInterfaceMembers(entry); valid.HasError())
                    return valid;
                std::vector<RuntimeResolvedGroupReference> resolved;
                resolved.reserve(entry.references.size());
                for (const auto &reference : entry.references) {
                    if (reference.ownerMember >= entry.members.size() || reference.property == 0 ||
                        std::ranges::any_of(resolved, [&](const auto &prior) {
                        return prior.owner == entry.members[reference.ownerMember] && prior.property == reference.property;
                    }))
                        return Result<void>::Failure(MakeError(SceneErrors::InvalidEntity));
                    RuntimeResolvedGroupReference output{entry.members[reference.ownerMember], reference.property, {}};
                    if (!ResolveInterfaceTarget(reference, group, entities, output))
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
            created.reserve(candidate.slots.size());
            for (std::size_t index = 0; index < candidate.slots.size(); ++index) {
                const auto &slot = candidate.slots[index];
                if (!slot.active || (index < scene.storage_.slots.size() && scene.storage_.slots[index].active &&
                                     scene.storage_.slots[index].generation == slot.generation))
                    continue;
                if (!slot.groupResources)
                    continue;
                resourceGroup = true;
                const EntityRef entity{scene.runtimeId_, {static_cast<std::uint32_t>(index), slot.generation}};
                const auto parent = slot.parent ? std::optional<EntityRef>{{scene.runtimeId_, *slot.parent}} : std::nullopt;
                created.emplace_back(entity, slot.authoredObject, parent, &slot.localTransform, &slot.primitiveMesh, &slot.components,
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
                })) {
                    const bool hasBaseline = std::ranges::any_of(candidate.slots, [](const auto &slot) {
                        return slot.active && slot.baselineOwner.has_value();
                    });
                    return Result<void>::Failure(
                        MakeError(hasBaseline ? SceneErrors::BaselineUnsupported : SceneErrors::AssetServicesUnavailable,
                                  "Required structural subsystem owner is not composed."));
                }
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

}  // namespace Horo::Runtime
