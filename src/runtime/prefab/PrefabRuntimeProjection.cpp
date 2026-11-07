#include "PrefabRuntimeProjection.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Runtime/Scene/BuiltinSceneComponents.h"

#include <algorithm>
#include <limits>
#include <set>

namespace Horo::Prefab::Detail {
    namespace {
        /** @brief Synchronous cooked-value projection with explicit immutable inputs
         * and exact target Scene. */
        struct Builder final {
            const CookedPrefabData &data;
            const PrefabSpawnRequest &request;
            Runtime::RuntimeSceneView scene;
            const Gameplay::ComponentRegistry *components;
            const PrefabComponentProjector *projector;
            std::uint64_t &lastBehaviorId;
            const std::optional<std::span<const PrefabRestoredBehaviorIdentity>> &restored;
            std::span<const PrefabRestoredBinding> restoredBindings;

            /** @brief Validates saved attachment addresses and exact installed type/schema before projection. */
            Result<void> ValidateRestoredIdentity(const PrefabRestoredBehaviorIdentity &identity) const {
                if (identity.source.entity.value >= data.entities.size())
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                const auto &members = data.entities[identity.source.entity.value].members;
                if (identity.source.member >= members.size())
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                const auto *behavior = std::get_if<Gameplay::BehaviorComponent>(&members[identity.source.member]);
                if (!behavior || !identity.runtime.value || behavior->typeId != identity.type ||
                    behavior->schemaVersion != identity.schemaVersion)
                    return Result<void>::Failure(
                        MakeError(PrefabErrors::OverrideConflict,
                                  "Saved behavior occurrence no longer matches the installed cooked type/schema."));
                return Result<void>::Success();
            }

            /** @brief Requires one complete unambiguous saved assignment per actual cooked behavior. */
            Result<void> ValidateRestoredIdentities() const {
                if (!restored)
                    return Result<void>::Success();
                std::set<std::pair<std::size_t, std::size_t>> sources;
                std::set<Gameplay::BehaviorInstanceId> identities;
                for (const auto &identity : *restored) {
                    if (auto valid = ValidateRestoredIdentity(identity); valid.HasError())
                        return valid;
                    if (!sources.emplace(identity.source.entity.value, identity.source.member).second ||
                        !identities.insert(identity.runtime).second)
                        return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                }
                std::size_t count{};
                for (const auto &entity : data.entities)
                    count += std::ranges::count_if(entity.members, [](const auto &member) {
                        return std::holds_alternative<Gameplay::BehaviorComponent>(member);
                    });
                if (count != restored->size())
                    return Result<void>::Failure(
                        MakeError(PrefabErrors::ReferenceRewriteInvalid,
                                  "Saved behavior identities do not completely cover the installed cooked occurrences."));
                return Result<void>::Success();
            }

            /** @brief Preserves exact saved IDs during reconstruction and the original fresh allocation otherwise. */
            Result<Gameplay::BehaviorInstanceId> BehaviorIdentity(std::size_t entity, std::size_t member) {
                if (restored) {
                    const auto found = std::ranges::find_if(*restored, [entity, member](const auto &identity) {
                        return identity.source.entity.value == entity && identity.source.member == member;
                    });
                    if (found == restored->end())
                        return Result<Gameplay::BehaviorInstanceId>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                    lastBehaviorId = std::max(lastBehaviorId, found->runtime.value);
                    return Result<Gameplay::BehaviorInstanceId>::Success(found->runtime);
                }
                if (lastBehaviorId == std::numeric_limits<std::uint64_t>::max())
                    return Result<Gameplay::BehaviorInstanceId>::Failure(MakeError(PrefabErrors::EntityAllocationExhausted));
                return Result<Gameplay::BehaviorInstanceId>::Success({++lastBehaviorId});
            }

            /** @brief Projects one exact raw member through the actual host codec or
             * registered component schema. */
            Result<void> ProjectRaw(const RawComponentPayload &raw, PrefabGroupProjection &projection, std::size_t entity) {
                auto &target = projection.components[entity];

                if (projector) {
                    auto projected = projector->Project(raw, target, projection.primitiveMeshes[entity]);
                    if (projected.HasError())
                        return Result<void>::Failure(projected.ErrorValue());
                    if (projected.Value()) {
                        projection.members[entity].emplace_back(raw.component.typeId, raw.instance.Value());
                        return Result<void>::Success();
                    }
                }
                if (Runtime::FindBuiltinSceneComponent(raw.component.typeId.Value()))
                    return Result<void>::Failure(MakeError(PrefabErrors::ComponentTypeUnregistered,
                                                           "A canonical built-in component requires the host's typed "
                                                           "projector."));
                if (!components || !components->IsFrozen())
                    return Result<void>::Failure(MakeError(PrefabErrors::ComponentTypeUnregistered));
                auto inspected = components->Inspect(raw.component);
                if (inspected.HasError())
                    return Result<void>::Failure(inspected.ErrorValue());
                if (inspected.Value().status != Gameplay::ComponentInspectionStatus::Current)
                    return Result<void>::Failure(MakeError(PrefabErrors::ComponentTypeUnregistered));
                target.gameplayComponents.push_back(raw.component);
                projection.members[entity].emplace_back(raw.component.typeId, raw.instance.Value());
                return Result<void>::Success();
            }

            /** @brief Validates declared coverage, supplied types and constraints before
             * constructing the detached candidate. */
            [[nodiscard]] Result<void> ValidateInitializations() const {
                for (const auto &input : request.initialization) {
                    const auto declaration = std::ranges::find(data.initialization, input.id, &CookedPrefabInitialization::id);
                    if (declaration == data.initialization.end())
                        return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Undeclared prefab initialization ID."));
                    if (auto valid = ValidatePrefabInitializationValue(*declaration, input.value); valid.HasError())
                        return valid;
                }
                for (const auto &declaration : data.initialization) {
                    if (declaration.required && std::ranges::find(request.initialization, declaration.id, &PrefabInitializationValue::id) ==
                                                    request.initialization.end())
                        return Result<void>::Failure(
                            MakeError(PrefabErrors::AdmissionRejected, "Required prefab initialization is missing."));
                }
                return Result<void>::Success();
            }

            /** @brief Validates actual already-live binding targets in the ordinary spawn path. */
            [[nodiscard]] Result<void> ValidateRuntimeBindings() const {
                const auto active = scene;
                for (const auto &input : request.bindings) {
                    const auto declaration = std::ranges::find(data.bindings, input.id, &CookedPrefabBindingDeclaration::id);
                    if (declaration == data.bindings.end())
                        return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                    auto target = active.Get(input.target);
                    if (target.HasError())
                        return Result<void>::Failure(target.ErrorValue());
                    if (declaration->componentType &&
                        !std::ranges::any_of(target.Value().components->gameplayComponents, [&](const auto &component) {
                        return component.typeId == *declaration->componentType;
                    }))
                        return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                }
                return Result<void>::Success();
            }

            /** @brief Required interfaces may resolve to already-live entities or complete admitted aggregate slots. */
            [[nodiscard]] Result<void> ValidateBindingCoverage() const {
                for (const auto &declaration : data.bindings) {
                    if (declaration.required &&
                        std::ranges::find(request.bindings, declaration.id, &PrefabRuntimeBinding::id) == request.bindings.end() &&
                        std::ranges::find(restoredBindings, declaration.id, &PrefabRestoredBinding::id) == restoredBindings.end())
                        return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                }
                return Result<void>::Success();
            }

            /** @brief Combines the same initialization, target and declared coverage checks. */
            [[nodiscard]] Result<void> ValidateInputs() const {
                if (auto valid = ValidateInitializations(); valid.HasError())
                    return valid;
                if (auto valid = ValidateRuntimeBindings(); valid.HasError())
                    return valid;
                return ValidateBindingCoverage();
            }

            /** @brief Projects one cooked member while retaining the original behavior
             * identity allocation order. */
            [[nodiscard]] Result<void> ProjectMember(PrefabGroupProjection &projection, const std::size_t entity,
                                                     const std::size_t member) {
                auto &target = projection.components[entity];
                const auto &source = data.entities[entity].members[member];
                if (const auto *raw = std::get_if<RawComponentPayload>(&source))
                    return ProjectRaw(*raw, projection, entity);
                auto identity = BehaviorIdentity(entity, member);
                if (identity.HasError())
                    return Result<void>::Failure(identity.ErrorValue());
                auto behavior = std::get<Gameplay::BehaviorComponent>(source);
                behavior.instanceId = identity.Value();
                for (const auto &declaration : data.initialization) {
                    if (declaration.owner.entity.value != entity || declaration.owner.member != member)
                        continue;
                    if (const auto input = std::ranges::find(request.initialization, declaration.id, &PrefabInitializationValue::id);
                        input != request.initialization.end())
                        behavior.fields[declaration.field].value = input->value;
                }
                projection.members[entity].emplace_back(behavior.typeId, behavior.instanceId.value);
                target.behaviors.push_back(std::move(behavior));
                return Result<void>::Success();
            }

            /** @brief Projects portable references after every cooked member identity has
             * been assigned. */
            void ProjectReferences(PrefabGroupProjection &projection) const {
                for (const auto &reference : data.references) {
                    Runtime::RuntimeGroupReference output{reference.owner.member, reference.property.Value(), {}};
                    std::visit([this, &output]<typename Target>(const Target &target) {
                        if constexpr (std::is_same_v<Target, CookedPrefabEntitySlot>)
                            output.target = Runtime::RuntimeGroupEntitySlot{target.value};
                        else if constexpr (std::is_same_v<Target, CookedPrefabMemberSlot>)
                            output.target = Runtime::RuntimeGroupMemberSlot{{target.entity.value}, target.member};
                        else if constexpr (std::is_same_v<Target, CookedPrefabAssetSlot>)
                            output.target = data.dependencies[target.value].asset;
                        else {
                            const auto &declaration = data.bindings[target.value];
                            if (const auto saved = std::ranges::find(restoredBindings, declaration.id, &PrefabRestoredBinding::id);
                                saved != restoredBindings.end()) {
                                output.target = saved->target;
                                return;
                            }
                            if (const auto input = std::ranges::find(request.bindings, declaration.id, &PrefabRuntimeBinding::id);
                                input != request.bindings.end())
                                output.target = Runtime::RuntimeGroupExternalReference{input->target, declaration.componentType};
                        }
                    }, reference.target);
                    projection.references[reference.owner.entity.value].push_back(std::move(output));
                }
            }

            /** @brief Resolves a body in the exact source occurrence; no numeric Scene-ID
             * inference crosses the prefab boundary. */
            static Result<Runtime::RuntimeGroupEntitySlot> ResolveBody(const CookedPrefabData &data,
                                                                       const PrefabGroupProjection &projection, const std::size_t owner,
                                                                       const Runtime::PhysicsBodyReference &reference) {
                const auto &source = data.entities[owner].provenance;
                std::optional<std::size_t> found;
                for (std::size_t index = 0; index < data.entities.size(); ++index) {
                    if (const auto &target = data.entities[index].provenance;
                        source.sourceAsset != target.sourceAsset || target.sourceObject.SourceObject().value != reference.object.value ||
                        !std::ranges::equal(source.sourceObject.NestedInstanceScope(), target.sourceObject.NestedInstanceScope()))
                        continue;
                    if (found.has_value())
                        return Result<Runtime::RuntimeGroupEntitySlot>::Failure(MakeError(PrefabErrors::CorruptedPayload));
                    found = index;
                }
                if (!found || !projection.components[*found].rigidBody || !projection.components[*found].rigidBody->enabled ||
                    projection.components[*found].rigidBody->body != reference.body)
                    return Result<Runtime::RuntimeGroupEntitySlot>::Failure(
                        MakeError(PrefabErrors::CorruptedPayload, "Prefab Physics endpoint has no exact enabled source body."));
                return Result<Runtime::RuntimeGroupEntitySlot>::Success({*found});
            }

            /** @brief Resolves and appends one typed endpoint, preserving the exact
             * source-body validation error. */
            static Result<void> AppendPhysicsReference(const CookedPrefabData &data, PrefabGroupProjection &projection,
                                                       const std::size_t entity, const Runtime::GroupPhysicsReferenceKind kind,
                                                       const std::size_t component, const Runtime::PhysicsBodyReference &reference) {
                auto target = ResolveBody(data, projection, entity, reference);
                if (target.HasError())
                    return Result<void>::Failure(target.ErrorValue());
                projection.physicsReferences[entity].emplace_back(kind, component, target.Value(), reference.body);
                return Result<void>::Success();
            }

            /** @brief Builds complete typed endpoint fixups before Scene reserves any
             * entity or native owner. */
            static Result<void> ProjectPhysicsReferences(const CookedPrefabData &data, PrefabGroupProjection &projection) {
                projection.physicsReferences.resize(data.entities.size());
                for (std::size_t index = 0; index < projection.components.size(); ++index) {
                    const auto &components = projection.components[index];
                    for (std::size_t collider = 0; collider < components.colliders.size(); ++collider) {
                        const auto &value = components.colliders[collider];
                        if (!value.enabled)
                            continue;
                        if (auto valid = AppendPhysicsReference(data, projection, index, Runtime::GroupPhysicsReferenceKind::ColliderBody,
                                                                collider, value.body);
                            valid.HasError())
                            return valid;
                    }
                    for (std::size_t constraint = 0; constraint < components.physicsConstraints.size(); ++constraint) {
                        const auto &value = components.physicsConstraints[constraint];
                        if (!value.enabled)
                            continue;
                        if (auto valid =
                                AppendPhysicsReference(data, projection, index, Runtime::GroupPhysicsReferenceKind::ConstraintFirst,
                                                       constraint, value.first.body);
                            valid.HasError())
                            return valid;
                        const auto *second = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&value.second);
                        if (!second)
                            continue;
                        if (auto valid =
                                AppendPhysicsReference(data, projection, index, Runtime::GroupPhysicsReferenceKind::ConstraintSecond,
                                                       constraint, second->body);
                            valid.HasError())
                            return valid;
                    }
                }
                return Result<void>::Success();
            }

            /** @brief Rejects malformed authored duplicates within one exact template
             * occurrence before AI remapping. Repeated nested placements have distinct
             * canonical scopes and may carry the same source-authored identity. */
            static Result<void> ValidateAgentSources(const CookedPrefabData &data, const PrefabGroupProjection &projection) {
                for (std::size_t index = 0; index < projection.components.size(); ++index) {
                    const auto &agent = projection.components[index].aiAgent;
                    if (!agent)
                        continue;
                    if (!agent->agent.IsValid())
                        return Result<void>::Failure(MakeError(PrefabErrors::CorruptedPayload));
                    const auto &source = data.entities[index].provenance;
                    for (std::size_t previous = 0; previous < index; ++previous) {
                        const auto &other = projection.components[previous].aiAgent;
                        const auto &origin = data.entities[previous].provenance;
                        if (other && other->agent == agent->agent && source.sourceAsset == origin.sourceAsset &&
                            std::ranges::equal(source.sourceObject.NestedInstanceScope(), origin.sourceObject.NestedInstanceScope()))
                            return Result<void>::Failure(MakeError(PrefabErrors::CorruptedPayload,
                                                                   "One prefab source occurrence contains duplicate "
                                                                   "authored AI identities."));
                    }
                }
                return Result<void>::Success();
            }

            /** @brief Reads the actual receiving Scene's maximum; no saved or global allocator replaces its cursor. */
            void ObserveBehaviorCursor() {
                const auto active = scene;
                for (std::size_t slot = 0; slot < active.SlotCount(); ++slot) {
                    if (const auto entity = active.EntityAt(slot)) {
                        for (const auto &behavior : entity->components->behaviors)
                            lastBehaviorId = std::max(lastBehaviorId, behavior.instanceId.value);
                    }
                }
            }

            /** @brief Reserves ordered projection containers and stops on the first typed
             * member failure. */
            [[nodiscard]] Result<PrefabGroupProjection> Project() {
                if (auto valid = ValidateRestoredIdentities(); valid.HasError())
                    return Result<PrefabGroupProjection>::Failure(valid.ErrorValue());
                if (auto valid = ValidateInputs(); valid.HasError())
                    return Result<PrefabGroupProjection>::Failure(valid.ErrorValue());
                ObserveBehaviorCursor();
                PrefabGroupProjection projection;
                projection.components.resize(data.entities.size());
                projection.primitiveMeshes.resize(data.entities.size());
                projection.members.resize(data.entities.size());
                projection.references.resize(data.entities.size());
                for (std::size_t entity = 0; entity < data.entities.size(); ++entity) {
                    for (std::size_t member = 0; member < data.entities[entity].members.size(); ++member) {
                        if (auto projected = ProjectMember(projection, entity, member); projected.HasError())
                            return Result<PrefabGroupProjection>::Failure(projected.ErrorValue());
                    }
                }
                if (auto valid = ValidateAgentSources(data, projection); valid.HasError())
                    return Result<PrefabGroupProjection>::Failure(valid.ErrorValue());
                if (auto valid = ProjectPhysicsReferences(data, projection); valid.HasError())
                    return Result<PrefabGroupProjection>::Failure(valid.ErrorValue());
                ProjectReferences(projection);
                return Result<PrefabGroupProjection>::Success(std::move(projection));
            }
        };
    }  // namespace

    /** @copydoc PreparePrefabGroupProjection */
    Result<PrefabGroupProjection> PreparePrefabGroupProjection(
        const CookedPrefabData &data, const PrefabSpawnRequest &request, Runtime::RuntimeSceneView scene,
        const Gameplay::ComponentRegistry *components, const PrefabComponentProjector *projector, std::uint64_t &lastBehaviorId,
        const std::optional<std::span<const PrefabRestoredBehaviorIdentity>> &restored,
        std::span<const PrefabRestoredBinding> restoredBindings) {
        return Builder{data, request, scene, components, projector, lastBehaviorId, restored, restoredBindings}.Project();
    }
}  // namespace Horo::Prefab::Detail
