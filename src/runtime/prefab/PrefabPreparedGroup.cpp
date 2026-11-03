#include "PrefabPreparedGroup.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>

namespace Horo::Prefab::Detail {
    namespace {
        /** @brief Preserves cooked schema/occurrence authority while allowing fresh
         * runtime behavior identities. */
        bool MatchesCookedMember(const CookedPrefabMember &source, const Runtime::RuntimeGroupMemberIdentity &projected) {
            if (const auto *raw = std::get_if<RawComponentPayload>(&source)) {
                const auto *type = std::get_if<Gameplay::ComponentTypeId>(&projected.type);
                return type && *type == raw->component.typeId && projected.instance == raw->instance.Value();
            }
            const auto *type = std::get_if<Gameplay::BehaviorTypeId>(&projected.type);
            return type && *type == std::get<Gameplay::BehaviorComponent>(source).typeId && projected.instance != 0;
        }

        /** @brief Keeps declared optionality/component type for the exact supplied external target. */
        bool MatchesExternalBinding(const CookedPrefabBindingDeclaration &binding, const Runtime::RuntimeGroupReference &projected) {
            if (std::holds_alternative<std::monostate>(projected.target))
                return !binding.required;
            const auto *value = std::get_if<Runtime::RuntimeGroupExternalReference>(&projected.target);
            return value && value->component == binding.componentType;
        }

        /** @brief Checks portable target preservation; Scene still admits actual
         * external entities at its final fence. */
        bool MatchesCookedReference(const CookedPrefabData &data, const CookedPrefabReference &source,
                                    const Runtime::RuntimeGroupReference &projected) {
            return std::visit([&data, &projected]<typename Target>(const Target &target) {
                if constexpr (std::is_same_v<Target, CookedPrefabEntitySlot>) {
                    const auto *value = std::get_if<Runtime::RuntimeGroupEntitySlot>(&projected.target);
                    return value && value->index == target.value;
                } else if constexpr (std::is_same_v<Target, CookedPrefabMemberSlot>) {
                    const auto *value = std::get_if<Runtime::RuntimeGroupMemberSlot>(&projected.target);
                    return value && value->entity.index == target.entity.value && value->member == target.member;
                } else if constexpr (std::is_same_v<Target, CookedPrefabAssetSlot>) {
                    const auto *value = std::get_if<Assets::AssetDependency>(&projected.target);
                    return value && value->id == data.dependencies[target.value].asset.id &&
                           value->expectedType == data.dependencies[target.value].asset.expectedType;
                } else {
                    return MatchesExternalBinding(data.bindings[target.value], projected);
                }
            }, source.target);
        }

        /** @brief Requires the exact complete member table for one receiving entity. */
        Result<void> ValidatePreparedMembers(const CookedPrefabData &data, const PrefabPreparedGroupOptions &options, std::size_t entity) {
            const auto &members = data.entities[entity].members;
            const auto projected = options.members.empty() ? std::span<const Runtime::RuntimeGroupMemberIdentity>{}
                                                           : std::span<const Runtime::RuntimeGroupMemberIdentity>{options.members[entity]};
            if (projected.size() != members.size())
                return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Incomplete cooked member projection."));
            for (std::size_t member = 0; member < members.size(); ++member)
                if (!MatchesCookedMember(members[member], projected[member]))
                    return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Cooked member identity changed."));
            return Result<void>::Success();
        }

        /** @brief Requires every original cooked reference in its exact canonical interface order. */
        Result<void> ValidatePreparedReferences(const CookedPrefabData &data, const PrefabPreparedGroupOptions &options, std::size_t entity,
                                                std::size_t &reference) {
            const auto projected = options.references.empty() ? std::span<const Runtime::RuntimeGroupReference>{}
                                                              : std::span<const Runtime::RuntimeGroupReference>{options.references[entity]};
            std::size_t index{};
            while (reference < data.references.size() && data.references[reference].owner.entity.value == entity) {
                const auto &source = data.references[reference++];
                if (index >= projected.size())
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                const auto &value = projected[index++];
                if (value.ownerMember != source.owner.member || value.property != source.property.Value() ||
                    !MatchesCookedReference(data, source, value))
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
            }
            return index == projected.size() ? Result<void>::Success()
                                             : Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
        }

        /** @brief Requires complete cooked interface coverage before any Scene queue transfer. */
        Result<void> ValidatePreparedInterfaces(const CookedPrefabData &data, const PrefabPreparedGroupOptions &options) {
            std::size_t reference{};
            for (std::size_t entity = 0; entity < data.entities.size(); ++entity) {
                if (auto valid = ValidatePreparedMembers(data, options, entity); valid.HasError())
                    return valid;
                if (auto valid = ValidatePreparedReferences(data, options, entity, reference); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MakePrefabGroupEntries */
    /** @brief Transfers dense projected components and metadata while preserving
     * cooked topology and ordering. */
    std::vector<Runtime::RuntimeEntityGroupEntry> MakePrefabGroupEntries(const std::span<const CookedPrefabEntity> entities,
                                                                         std::vector<Runtime::RuntimeComponentSet> components,
                                                                         PrefabPreparedGroupOptions &options) {
        std::vector<Runtime::RuntimeEntityGroupEntry> entries;
        entries.reserve(entities.size());
        for (std::size_t index = 0; index < entities.size(); ++index) {
            Runtime::RuntimeEntityGroupEntry entry;
            entry.info.localTransform = entities[index].localTransform;
            entry.info.components = std::move(components[index]);
            if (!options.primitiveMeshes.empty())
                entry.info.primitiveMesh = std::move(options.primitiveMeshes[index]);
            if (!options.physicsReferences.empty())
                entry.physicsReferences = std::move(options.physicsReferences[index]);
            if (!options.members.empty())
                entry.members = std::move(options.members[index]);
            if (!options.references.empty())
                entry.references = std::move(options.references[index]);
            entry.spawnLineage = options.spawnLineage;
            if (entities[index].parent)
                entry.parentInGroup = entities[index].parent->value;
            entries.push_back(std::move(entry));
        }
        if (options.rootPlacement)
            entries.front().info.localTransform = *options.rootPlacement;
        entries.front().info.parent = options.parent;
        return entries;
    }

    /** @copydoc PreparePrefabGroupCommands */
    Result<PrefabPreparedGroup> PreparePrefabGroupCommands(const PreparedPrefabSpawn &prepared,
                                                           std::vector<Runtime::RuntimeComponentSet> components,
                                                           PrefabPreparedGroupOptions options,
                                                           Runtime::SceneStructuralAdmission admission) {
        const auto &lease = prepared.Source();
        const auto &data = prepared.Data();
        if (!lease.Template() || lease.Template()->Data().assetId != data.assetId || components.size() != data.entities.size())
            return Result<PrefabPreparedGroup>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        if (auto interfaces = ValidatePreparedInterfaces(data, options); interfaces.HasError())
            return Result<PrefabPreparedGroup>::Failure(interfaces.ErrorValue());
        if (prepared.OverridesRootTransform())
            options.rootPlacement.reset();
        auto entries = MakePrefabGroupEntries(data.entities, std::move(components), options);
        std::vector<Runtime::RuntimeGroupAssetLease> resources;
        resources.reserve(lease.Dependencies().size() + 1);
        resources.emplace_back(Assets::AssetDependency{lease.Template()->Data().assetId, Assets::AssetTypeId::Parse("core.prefab").Value()},
                               lease.Artifact());
        for (const auto &dependency : lease.Dependencies())
            resources.emplace_back(dependency.metadata, dependency.artifact);
        PrefabPreparedGroup group;
        auto tokens = group.commands.CreateGroup(std::move(entries), std::move(resources), std::move(admission));
        if (tokens.HasError())
            return Result<PrefabPreparedGroup>::Failure(tokens.ErrorValue());
        group.entities = std::move(tokens).Value();
        return Result<PrefabPreparedGroup>::Success(std::move(group));
    }
}  // namespace Horo::Prefab::Detail
