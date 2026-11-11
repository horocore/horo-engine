#include "Horo/Prefab/PrefabRestoreEntities.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabPersistentCodec.h"
#include "Horo/Prefab/PreparedPrefabSpawn.h"
#include "PrefabPreparedGroup.h"
#include "PrefabRuntimeProjection.h"

#include <algorithm>
#include <map>
#include <new>
#include <set>

namespace Horo::Prefab {
    namespace {
        template <typename T> Result<T> Invalid(const char *message) {
            return Result<T>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid, message));
        }

        bool Live(const Runtime::PersistentEntityRecord &record) noexcept {
            return record.disposition == Runtime::PersistentEntityDisposition::Live;
        }

        bool SameRecord(const Runtime::PersistentEntityRecord &a, const Runtime::PersistentEntityRecord &b) noexcept {
            return a.identity == b.identity && a.generation == b.generation && a.provenance == b.provenance &&
                   a.disposition == b.disposition;
        }
    }  // namespace

    struct PrefabRestoreEntities::State final {
        struct Node final {
            const PrefabPersistentMember *member{};
            std::size_t instance{};
            std::size_t local{};
            std::optional<std::size_t> dense;
        };

        State(PrefabPersistentState saved, std::vector<PrefabTemplateLease> leases, PrefabTemplateProvider &provider,
              const Gameplay::ComponentRegistry *components, const PrefabComponentProjector *projector,
              std::shared_ptr<const PrefabValueSchemaSnapshot> schemas, PrefabPersistenceLimits limits, CancellationToken cancellation)
            : saved(std::move(saved)), leases(std::move(leases)), provider(provider), components(components), projector(projector),
              schemas(std::move(schemas)), limits(limits), cancellation(std::move(cancellation)) {}

        Result<void> ValidateLease(std::size_t index) const {
            const auto &recipe = saved.instances[index];
            if (recipe.disposition == Runtime::PersistentEntityDisposition::Tombstone)
                return Result<void>::Success();
            const auto &lease = leases[index];
            if (!lease.Template() || !lease.Target() || lease.Template()->Data().assetId != recipe.source.asset ||
                lease.Artifact().Digest() != recipe.source.artifactDigest || *lease.Target() != recipe.source.target ||
                lease.Template()->Data().rootSourceRevision != recipe.rootSourceRevision)
                return Invalid<void>("Installed cooked prefab does not match the exact saved artifact, target or root revision.");
            return provider.ValidateAdmission(lease);
        }

        Result<void> ValidateLeases() const {
            if (cancelled || cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
            for (std::size_t index = 0; index < leases.size(); ++index)
                if (auto valid = ValidateLease(index); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }

        Result<void> PrepareDescriptors() {
            prepared.resize(saved.instances.size());
            localNodes.resize(saved.instances.size());
            for (std::size_t index = 0; index < saved.instances.size(); ++index) {
                const auto &recipe = saved.instances[index];
                if (recipe.disposition == Runtime::PersistentEntityDisposition::Tombstone)
                    continue;
                auto value =
                    PreparedPrefabSpawn::Prepare(leases[index], recipe.overrides, schemas.get(), provider.LimitProfile(), cancellation);
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                if (value.Value().OverridesRootTransform())
                    return Invalid<void>("Saved root placement and root-transform delta cannot both own the same field.");
                prepared[index].emplace(std::move(value).Value());
                if (auto valid = IndexMembers(index); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        Result<void> IndexMembers(std::size_t index) {
            const auto &recipe = saved.instances[index];
            const auto &entities = prepared[index]->Data().entities;
            if (entities.size() != recipe.members.size())
                return Invalid<void>("Saved member inventory no longer covers the exact installed cooked prefab.");
            auto &local = localNodes[index];
            local.reserve(entities.size());
            for (std::size_t entity = 0; entity < entities.size(); ++entity) {
                const auto found = std::ranges::find(recipe.members, entities[entity].provenance, &PrefabPersistentMember::source);
                if (found == recipe.members.end())
                    return Invalid<void>("An installed cooked member has no exact saved provenance.");
                const auto [inserted, unique] = nodes.emplace(found->record.identity, Node{&*found, index, entity, {}});
                if (!unique)
                    return Invalid<void>("Repeated stable member identity in reconstruction inventory.");
                local.push_back(&inserted->second);
            }
            return Result<void>::Success();
        }

        Result<void> ValidateRecords(std::span<const Runtime::PersistentEntityRecord> records) const {
            std::map<Runtime::PersistentEntityId, const Runtime::PersistentEntityRecord *> admitted;
            for (const auto &record : records)
                if (!admitted.emplace(record.identity, &record).second)
                    return Invalid<void>("Repeated aggregate entity identity.");
            std::size_t spawned{};
            for (const auto &record : records)
                spawned += std::holds_alternative<Runtime::SpawnedPersistentEntityProvenance>(record.provenance);
            std::size_t members{};
            for (const auto &recipe : saved.instances)
                for (const auto &member : recipe.members) {
                    const auto found = admitted.find(member.record.identity);
                    if (found == admitted.end() || !SameRecord(member.record, *found->second))
                        return Invalid<void>("Prefab recipe and sole aggregate entity inventory disagree.");
                    ++members;
                }
            if (members != spawned)
                return Invalid<void>("Aggregate spawned inventory is not completely owned by the saved prefab recipes.");
            return Result<void>::Success();
        }

        Result<const Runtime::PersistentEntityRecord *> ReferenceRecord(const PrefabPersistentEntityReference &reference) const {
            const auto found = records.find(reference.target.entity);
            if (reference.target.world != saved.world || found == records.end() || found->second.generation != reference.generation ||
                !Live(found->second))
                return Invalid<const Runtime::PersistentEntityRecord *>(
                    "Saved prefab relationship targets an absent, stale or tombstoned incarnation.");
            return Result<const Runtime::PersistentEntityRecord *>::Success(&found->second);
        }

        Result<bool> ParentReady(std::size_t index, const std::set<std::size_t> &ordered) const {
            const auto &parent = saved.instances[index].parent;
            if (!parent)
                return Result<bool>::Success(true);
            auto record = ReferenceRecord(*parent);
            if (record.HasError())
                return Result<bool>::Failure(record.ErrorValue());
            const auto target = nodes.find(record.Value()->identity);
            if (target == nodes.end())
                return Result<bool>::Success(true);
            return Result<bool>::Success(ordered.contains(target->second.instance));
        }

        Result<void> OrderInstances() {
            std::set<std::size_t> remaining;
            std::set<std::size_t> ordered;
            for (std::size_t index = 0; index < prepared.size(); ++index)
                if (prepared[index])
                    remaining.insert(index);
            while (!remaining.empty()) {
                const auto before = remaining.size();
                for (auto current = remaining.begin(); current != remaining.end();) {
                    auto ready = ParentReady(*current, ordered);
                    if (ready.HasError())
                        return Result<void>::Failure(ready.ErrorValue());
                    if (!ready.Value()) {
                        ++current;
                        continue;
                    }
                    order.push_back(*current);
                    ordered.insert(*current);
                    current = remaining.erase(current);
                }
                if (before == remaining.size())
                    return Invalid<void>("Saved prefab parent hierarchy contains a cycle.");
            }
            return Result<void>::Success();
        }

        Result<void> AssignDenseSlots(std::size_t maximumEntities) {
            for (const auto index : order) {
                const auto &data = prepared[index]->Data();
                if (!Live(localNodes[index][0]->member->record))
                    return Invalid<void>("A live saved occurrence requires its exact live cooked root.");
                for (std::size_t entity = 0; entity < data.entities.size(); ++entity) {
                    auto &node = *localNodes[index][entity];
                    if (!Live(node.member->record))
                        continue;
                    if (data.entities[entity].parent && !localNodes[index][data.entities[entity].parent->value]->dense.has_value())
                        return Invalid<void>("A live saved member cannot have a tombstoned cooked parent.");
                    if (denseNodes.size() >= maximumEntities)
                        return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
                    node.dense = denseNodes.size();
                    denseNodes.push_back(&node);
                }
            }
            return Result<void>::Success();
        }

        Result<Runtime::EntityRef> AuthoredTarget(const Runtime::PersistentEntityRecord &record, Runtime::RuntimeSceneView scene) const {
            const auto *authored = std::get_if<Runtime::AuthoredPersistentEntityProvenance>(&record.provenance);
            if (!authored || authored->scene != scene.DefinitionId())
                return Invalid<Runtime::EntityRef>("Saved relationship lacks an exact authored or admitted spawned target.");
            const auto target = scene.Find(authored->object);
            if (!target)
                return Invalid<Runtime::EntityRef>("Saved authored relationship target is absent from the private Scene.");
            return Result<Runtime::EntityRef>::Success(*target);
        }

        Result<Detail::PrefabRestoredBinding> ResolveBinding(const PrefabPersistentBinding &binding,
                                                             const CookedPrefabBindingDeclaration &declaration,
                                                             Runtime::RuntimeSceneView scene) const {
            auto record = ReferenceRecord(binding.target);
            if (record.HasError())
                return Result<Detail::PrefabRestoredBinding>::Failure(record.ErrorValue());
            const auto dynamic = nodes.find(record.Value()->identity);
            if (dynamic != nodes.end())
                return DynamicBinding(binding.slot, declaration, dynamic->second);
            auto target = AuthoredTarget(*record.Value(), scene);
            if (target.HasError())
                return Result<Detail::PrefabRestoredBinding>::Failure(target.ErrorValue());
            if (declaration.componentType) {
                const auto entity = scene.Get(target.Value());
                if (entity.HasError() || !std::ranges::any_of(entity.Value().components->gameplayComponents, [&](const auto &component) {
                    return component.typeId == *declaration.componentType;
                }))
                    return Invalid<Detail::PrefabRestoredBinding>("Saved external component binding lost its required target schema.");
            }
            return Result<Detail::PrefabRestoredBinding>::Success(
                {binding.slot, Runtime::RuntimeGroupExternalReference{target.Value(), declaration.componentType}});
        }

        Result<Detail::PrefabRestoredBinding> DynamicBinding(PrefabPropertyId slot, const CookedPrefabBindingDeclaration &declaration,
                                                             const Node &node) const {
            if (!node.dense.has_value())
                return Invalid<Detail::PrefabRestoredBinding>("Saved external binding targets a deleted dynamic member.");
            if (!declaration.componentType)
                return Result<Detail::PrefabRestoredBinding>::Success({slot, Runtime::RuntimeGroupExternalEntitySlot{{*node.dense}, {}}});
            const auto &members = prepared[node.instance]->Data().entities[node.local].members;
            const auto found = std::ranges::find_if(members, [&](const auto &member) {
                const auto *raw = std::get_if<RawComponentPayload>(&member);
                return raw && raw->component.typeId == *declaration.componentType;
            });
            if (found == members.end())
                return Invalid<Detail::PrefabRestoredBinding>("Saved dynamic component binding lost its required installed target schema.");
            return Result<Detail::PrefabRestoredBinding>::Success(
                {slot, Runtime::RuntimeGroupExternalEntitySlot{{*node.dense}, declaration.componentType}});
        }

        Result<std::vector<Detail::PrefabRestoredBinding>> Bindings(std::size_t index, Runtime::RuntimeSceneView scene) const {
            std::vector<Detail::PrefabRestoredBinding> output;
            const auto &data = prepared[index]->Data();
            for (const auto &binding : saved.instances[index].bindings) {
                const auto declaration = std::ranges::find(data.bindings, binding.slot, &CookedPrefabBindingDeclaration::id);
                if (declaration == data.bindings.end())
                    return Invalid<std::vector<Detail::PrefabRestoredBinding>>(
                        "Saved binding interface was removed from the installed prefab.");
                auto value = ResolveBinding(binding, *declaration, scene);
                if (value.HasError())
                    return Result<std::vector<Detail::PrefabRestoredBinding>>::Failure(value.ErrorValue());
                output.push_back(std::move(value).Value());
            }
            return Result<std::vector<Detail::PrefabRestoredBinding>>::Success(std::move(output));
        }

        Result<std::vector<Detail::PrefabRestoredBehaviorIdentity>> Behaviors(std::size_t index) {
            std::vector<Detail::PrefabRestoredBehaviorIdentity> output;
            const auto &data = prepared[index]->Data();
            for (std::size_t entity = 0; entity < data.entities.size(); ++entity) {
                const auto &savedMember = *localNodes[index][entity]->member;
                std::size_t count{};
                for (std::size_t member = 0; member < data.entities[entity].members.size(); ++member) {
                    const auto *behavior = std::get_if<Gameplay::BehaviorComponent>(&data.entities[entity].members[member]);
                    if (!behavior)
                        continue;
                    auto identity = Behavior(savedMember, *behavior);
                    if (identity.HasError())
                        return Result<std::vector<Detail::PrefabRestoredBehaviorIdentity>>::Failure(identity.ErrorValue());
                    output.push_back({{{static_cast<std::uint32_t>(entity)}, static_cast<std::uint32_t>(member)},
                                      behavior->typeId,
                                      identity.Value(),
                                      behavior->schemaVersion});
                    ++count;
                }
                if (count != savedMember.behaviors.size())
                    return Invalid<std::vector<Detail::PrefabRestoredBehaviorIdentity>>(
                        "Saved behavior occurrence coverage changed in installed content.");
            }
            return Result<std::vector<Detail::PrefabRestoredBehaviorIdentity>>::Success(std::move(output));
        }

        Result<Gameplay::BehaviorInstanceId> Behavior(const PrefabPersistentMember &member, const Gameplay::BehaviorComponent &source) {
            const auto found = std::ranges::find(member.behaviors, source.instanceId, &PrefabPersistentBehaviorIdentity::source);
            if (found == member.behaviors.end() || found->type != source.typeId || found->schemaVersion != source.schemaVersion)
                return Invalid<Gameplay::BehaviorInstanceId>(
                    "Saved behavior source/type/schema does not match the installed exact occurrence.");
            // Tombstoned occurrences never become runtime attachments and need not collide with live identities.
            if (Live(member.record) && !behaviorIds.insert(found->runtime).second)
                return Invalid<Gameplay::BehaviorInstanceId>("Saved behavior identity collides with another live attachment.");
            return Result<Gameplay::BehaviorInstanceId>::Success(found->runtime);
        }

        Result<std::size_t> Dense(std::size_t instance, std::size_t local) const {
            if (local >= localNodes[instance].size() || !localNodes[instance][local]->dense.has_value())
                return Invalid<std::size_t>("Cooked relationship targets an absent or tombstoned saved member.");
            return Result<std::size_t>::Success(*localNodes[instance][local]->dense);
        }

        Result<void> RemapReference(Runtime::RuntimeGroupReference &reference, std::size_t instance) const {
            if (auto *entity = std::get_if<Runtime::RuntimeGroupEntitySlot>(&reference.target)) {
                auto slot = Dense(instance, entity->index);
                if (slot.HasError())
                    return Result<void>::Failure(slot.ErrorValue());
                entity->index = slot.Value();
            } else if (auto *member = std::get_if<Runtime::RuntimeGroupMemberSlot>(&reference.target)) {
                auto slot = Dense(instance, member->entity.index);
                if (slot.HasError())
                    return Result<void>::Failure(slot.ErrorValue());
                member->entity.index = slot.Value();
            }
            return Result<void>::Success();
        }

        Result<void> RemapReferences(Runtime::RuntimeEntityGroupEntry &entry, std::size_t instance, std::size_t local) const {
            const auto &data = prepared[instance]->Data();
            for (auto &reference : entry.references) {
                const auto source = std::ranges::find_if(data.references, [&](const auto &value) {
                    return value.owner.entity.value == local && value.owner.member == reference.ownerMember &&
                           value.property.Value() == reference.property;
                });
                if (source == data.references.end())
                    return Invalid<void>("Projected reference has no original cooked interface.");
                // Saved external bindings already use aggregate slots, so only actual prefab-local targets are remapped.
                if (std::holds_alternative<CookedPrefabBindingSlot>(source->target))
                    continue;
                if (auto mapped = RemapReference(reference, instance); mapped.HasError())
                    return mapped;
            }
            return Result<void>::Success();
        }

        Result<void> RemapEntry(Runtime::RuntimeEntityGroupEntry &entry, std::size_t instance, std::size_t local,
                                Runtime::RuntimeSceneView scene) const {
            if (entry.parentInGroup.has_value()) {
                auto parent = Dense(instance, *entry.parentInGroup);
                if (parent.HasError())
                    return Result<void>::Failure(parent.ErrorValue());
                entry.parentInGroup = parent.Value();
            } else if (local == 0 && saved.instances[instance].parent) {
                auto parent = ReferenceRecord(*saved.instances[instance].parent);
                if (parent.HasError())
                    return Result<void>::Failure(parent.ErrorValue());
                const auto dynamic = nodes.find(parent.Value()->identity);
                if (dynamic != nodes.end())
                    entry.parentInGroup = dynamic->second.dense;
                else {
                    auto authored = AuthoredTarget(*parent.Value(), scene);
                    if (authored.HasError())
                        return Result<void>::Failure(authored.ErrorValue());
                    entry.info.parent = authored.Value();
                }
            }
            return RemapPhysics(entry, instance);
        }

        Result<void> RemapPhysics(Runtime::RuntimeEntityGroupEntry &entry, std::size_t instance) const {
            for (auto &reference : entry.physicsReferences) {
                auto *target = std::get_if<Runtime::RuntimeGroupEntitySlot>(&reference.target);
                if (!target)
                    return Invalid<void>("Private cooked Physics projection unexpectedly contains an external runtime target.");
                auto mapped = Dense(instance, target->index);
                if (mapped.HasError())
                    return Result<void>::Failure(mapped.ErrorValue());
                target->index = mapped.Value();
            }
            return Result<void>::Success();
        }

        Result<void> AppendInstance(std::size_t index, Runtime::RuntimeSceneView scene,
                                    std::vector<Runtime::RuntimeEntityGroupEntry> &entries) {
            auto bindings = Bindings(index, scene);
            if (bindings.HasError())
                return Result<void>::Failure(bindings.ErrorValue());
            auto identities = Behaviors(index);
            if (identities.HasError())
                return Result<void>::Failure(identities.ErrorValue());
            PrefabSpawnRequest request;
            request.initialization = saved.instances[index].initialization;
            auto projection =
                Detail::PreparePrefabGroupProjection(prepared[index]->Data(), request, scene, components, projector, lastBehavior,
                                                     std::span<const Detail::PrefabRestoredBehaviorIdentity>{identities.Value()},
                                                     bindings.Value());
            if (projection.HasError())
                return Result<void>::Failure(projection.ErrorValue());
            PrefabPreparedGroupOptions options;
            options.rootPlacement = saved.instances[index].placement;
            options.spawnLineage = saved.instances[index].creationLineage;
            options.primitiveMeshes = std::move(projection.Value().primitiveMeshes);
            options.members = std::move(projection.Value().members);
            options.references = std::move(projection.Value().references);
            options.physicsReferences = std::move(projection.Value().physicsReferences);
            auto localEntries =
                Detail::MakePrefabGroupEntries(prepared[index]->Data().entities, std::move(projection.Value().components), options);
            for (std::size_t local = 0; local < localEntries.size(); ++local) {
                if (!localNodes[index][local]->dense.has_value())
                    continue;
                auto &entry = localEntries[local];
                if (auto mapped = RemapEntry(entry, index, local, scene); mapped.HasError())
                    return mapped;
                if (auto mapped = RemapReferences(entry, index, local); mapped.HasError())
                    return mapped;
                entries.push_back(std::move(entry));
            }
            return Result<void>::Success();
        }

        Result<void> AddResource(Assets::AssetDependency metadata, Assets::AssetPayloadLease lease,
                                 std::vector<Runtime::RuntimeGroupAssetLease> &resources) const {
            const auto found = std::ranges::find_if(resources, [&](const auto &resource) {
                return resource.metadata.id == metadata.id;
            });
            if (found != resources.end()) {
                if (found->metadata.expectedType != metadata.expectedType || found->artifact.Digest() != lease.Digest())
                    return Invalid<void>("Aggregate prefab recipes disagree about one exact installed asset revision/type.");
                return Result<void>::Success();
            }
            resources.emplace_back(std::move(metadata), std::move(lease));
            return Result<void>::Success();
        }

        Result<std::vector<Runtime::RuntimeGroupAssetLease>> Resources() const {
            std::vector<Runtime::RuntimeGroupAssetLease> resources;
            for (const auto index : order) {
                const auto &lease = leases[index];
                if (auto valid = AddResource({saved.instances[index].source.asset, Assets::AssetTypeId::Parse("core.prefab").Value()},
                                             lease.Artifact(), resources);
                    valid.HasError())
                    return Result<std::vector<Runtime::RuntimeGroupAssetLease>>::Failure(valid.ErrorValue());
                for (const auto &dependency : lease.Dependencies())
                    if (auto valid = AddResource(dependency.metadata, dependency.artifact, resources); valid.HasError())
                        return Result<std::vector<Runtime::RuntimeGroupAssetLease>>::Failure(valid.ErrorValue());
            }
            return Result<std::vector<Runtime::RuntimeGroupAssetLease>>::Success(std::move(resources));
        }

        Result<void> InitializePreparation(Runtime::RuntimeSceneView scene, std::span<const Runtime::PersistentEntityRecord> incoming,
                                           std::size_t maximumEntities) {
            if (auto valid = ValidateRecords(incoming); valid.HasError())
                return valid;
            for (const auto &record : incoming)
                records.emplace(record.identity, record);
            if (auto valid = OrderInstances(); valid.HasError())
                return valid;
            if (auto valid = AssignDenseSlots(maximumEntities); valid.HasError())
                return valid;
            for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot)
                if (const auto entity = scene.EntityAt(slot))
                    for (const auto &behavior : entity->components->behaviors)
                        behaviorIds.insert(behavior.instanceId);
            instances.reserve(saved.instances.size());
            for (std::size_t index = 0; index < saved.instances.size(); ++index)
                instances.push_back({saved.instances[index].identity, leases[index],
                                     std::vector<Runtime::EntityRef>(saved.instances[index].members.size())});
            return Result<void>::Success();
        }

        /** @brief Derives complete qualified live partition declarations from the exact validated recipes and private dense map. */
        std::vector<Runtime::SceneRestoreGroupPartition> Partitions(std::vector<Runtime::RuntimeEntityGroupEntry> &entries) const {
            std::vector<Runtime::SceneRestoreGroupPartition> partitions;
            partitions.reserve(order.size());
            for (const auto instance : order) {
                Runtime::SceneRestoreGroupPartition partition{saved.world,
                                                              saved.instances[instance].identity,
                                                              *localNodes[instance][0]->dense,
                                                              {}};
                partition.members.reserve(localNodes[instance].size());
                for (const auto *node : localNodes[instance]) {
                    if (!node->dense.has_value())
                        continue;
                    const auto &record = node->member->record;
                    entries[*node->dense].restorePartition = partitions.size();
                    partition.members.push_back({*node->dense, record.identity, record.generation.value});
                }
                partitions.push_back(std::move(partition));
            }
            return partitions;
        }

        Result<std::vector<Runtime::PersistentEntityRuntimeBinding>> Materialize(Runtime::RuntimeScene &scene,
                                                                                 Runtime::SceneRestoreCandidateCommands &commands) {
            using Outcome = Result<std::vector<Runtime::PersistentEntityRuntimeBinding>>;
            std::vector<Runtime::RuntimeEntityGroupEntry> entries;
            entries.reserve(denseNodes.size());
            for (const auto index : order)
                if (auto valid = AppendInstance(index, scene.View(), entries); valid.HasError())
                    return Outcome::Failure(valid.ErrorValue());
            std::vector<Runtime::PersistentEntityRuntimeBinding> bindings;
            bindings.reserve(denseNodes.size());
            for (const auto *node : denseNodes)
                bindings.push_back({node->member->record.identity, node->member->record.generation, {}});
            if (entries.empty())
                return Outcome::Success(std::move(bindings));
            auto resources = Resources();
            if (resources.HasError())
                return Outcome::Failure(resources.ErrorValue());
            Runtime::SceneCommandBuffer batch;
            auto partitions = Partitions(entries);
            auto tokens =
                commands.CreateGroup(batch, std::move(entries), std::move(resources).Value(), cancellation, std::move(partitions));
            if (tokens.HasError())
                return Outcome::Failure(tokens.ErrorValue());
            auto committed = commands.Commit(batch);
            if (committed.HasError())
                return Outcome::Failure(committed.ErrorValue());
            if (auto valid = AdoptCreated(tokens.Value(), committed.Value(), bindings); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            return Outcome::Success(std::move(bindings));
        }

        Result<void> AdoptCreated(std::span<const Runtime::DeferredEntity> tokens, const Runtime::StructuralCommitResult &commit,
                                  std::vector<Runtime::PersistentEntityRuntimeBinding> &bindings) {
            if (tokens.size() != denseNodes.size() || commit.created.size() != tokens.size())
                return Invalid<void>("Scene group receipt does not cover the complete admitted prefab inventory.");
            for (std::size_t index = 0; index < tokens.size(); ++index) {
                const auto found = std::ranges::find(commit.created, tokens[index], &Runtime::DeferredEntityResolution::deferred);
                if (found == commit.created.end())
                    return Invalid<void>("Scene group receipt lacks an exact reserved member token.");
                bindings[index].runtime = found->entity;
                const auto &node = *denseNodes[index];
                const auto &members = saved.instances[node.instance].members;
                instances[node.instance].members[static_cast<std::size_t>(node.member - members.data())] = found->entity;
            }
            return Result<void>::Success();
        }

        PrefabPersistentState saved;
        std::vector<PrefabTemplateLease> leases;
        PrefabTemplateProvider &provider;
        const Gameplay::ComponentRegistry *components;
        const PrefabComponentProjector *projector;
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas;
        PrefabPersistenceLimits limits;
        CancellationToken cancellation;
        std::vector<std::optional<PreparedPrefabSpawn>> prepared;
        std::map<Runtime::PersistentEntityId, Node> nodes;
        std::map<Runtime::PersistentEntityId, Runtime::PersistentEntityRecord> records;
        std::vector<std::vector<Node *>> localNodes;
        std::vector<Node *> denseNodes;
        std::vector<std::size_t> order;
        std::set<Gameplay::BehaviorInstanceId> behaviorIds;
        std::uint64_t lastBehavior{};
        std::vector<PrefabRestoredInstance> instances;
        bool started{};
        bool ready{};
        bool published{};
        bool cancelled{};
    };

    /** @copydoc PrefabRestoreEntities::Create */
    Result<std::shared_ptr<PrefabRestoreEntities>> PrefabRestoreEntities::Create(
        PrefabPersistentState saved, std::vector<PrefabTemplateLease> leases, PrefabTemplateProvider &provider,
        const Gameplay::ComponentRegistry *components, const PrefabComponentProjector *projector,
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas, PrefabPersistenceLimits limits, CancellationToken cancellation) {
        using Outcome = Result<std::shared_ptr<PrefabRestoreEntities>>;
        try {
            if (auto valid = EncodePrefabPersistentState(saved, limits, provider.LimitProfile()); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            if (leases.size() != saved.instances.size())
                return Invalid<std::shared_ptr<PrefabRestoreEntities>>("Restore requires one exact lease position per durable recipe.");
            auto state = std::make_unique<State>(std::move(saved), std::move(leases), provider, components, projector, std::move(schemas),
                                                 limits, std::move(cancellation));
            if (auto valid = state->ValidateLeases(); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            if (auto valid = state->PrepareDescriptors(); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            return Outcome::Success(std::shared_ptr<PrefabRestoreEntities>(new PrefabRestoreEntities(std::move(state))));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PrefabRestoreEntities::Prepare */
    Result<std::vector<Runtime::PersistentEntityRuntimeBinding>> PrefabRestoreEntities::Prepare(
        Runtime::RuntimeScene &scene, std::span<const Runtime::PersistentEntityRecord> records, std::size_t maximumEntities,
        Runtime::SceneRestoreCandidateCommands &commands) {
        using Outcome = Result<std::vector<Runtime::PersistentEntityRuntimeBinding>>;
        auto &state = *state_;
        if (state.started || maximumEntities == 0 || records.size() > maximumEntities)
            return Invalid<std::vector<Runtime::PersistentEntityRuntimeBinding>>(
                "Private prefab reconstruction is single-use and aggregate bounded.");
        state.started = true;
        if (auto valid = state.ValidateLeases(); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (auto valid = state.InitializePreparation(scene.View(), records, maximumEntities); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        auto bindings = state.Materialize(scene, commands);
        state.ready = bindings.HasValue();
        return bindings;
    }

    /** @copydoc PrefabRestoreEntities::ValidatePublication */
    Result<void> PrefabRestoreEntities::ValidatePublication() const {
        if (!state_->ready || state_->published)
            return Invalid<void>("Prefab reconstruction has not reached the original aggregate publication gate.");
        return state_->ValidateLeases();
    }

    /** @copydoc PrefabRestoreEntities::PublishPrepared */
    void PrefabRestoreEntities::PublishPrepared() noexcept {
        state_->published = true;
    }

    /** @copydoc PrefabRestoreEntities::Cancel */
    void PrefabRestoreEntities::Cancel() noexcept {
        if (state_->published)
            return;
        state_->cancelled = true;
        state_->ready = false;
    }

    /** @copydoc PrefabRestoreEntities::PreparedInstances */
    std::span<const PrefabRestoredInstance> PrefabRestoreEntities::PreparedInstances() const noexcept {
        return state_->ready ? std::span<const PrefabRestoredInstance>{state_->instances} : std::span<const PrefabRestoredInstance>{};
    }

    /** @copydoc PrefabRestoreEntities::PrefabRestoreEntities */
    PrefabRestoreEntities::PrefabRestoreEntities(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    PrefabRestoreEntities::~PrefabRestoreEntities() = default;
}  // namespace Horo::Prefab
