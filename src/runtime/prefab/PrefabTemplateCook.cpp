#include "Horo/Prefab/PrefabTemplateCook.h"

#include <algorithm>
#include <utility>

namespace Horo::Prefab {
    namespace {
        /** @brief Admits exactly one captured canonical resource and computes its actual full-envelope digest. */
        [[nodiscard]] Result<CookedPrefabDependency> ResourceDependency(const PrefabDependencyNode &node,
                                                                        const std::span<const PrefabTemplateCookResource> resources,
                                                                        const AssetCookTargetId &target,
                                                                        const Assets::AssetCookLimits &resourceLimits) {
            const PrefabTemplateCookResource *found{};
            for (const auto &resource : resources) {
                if (resource.asset != node.assetId)
                    continue;
                if (found != nullptr)
                    return Result<CookedPrefabDependency>::Failure(MakeError(PrefabErrors::DependencyGraphInvalid));
                found = &resource;
            }
            if (found == nullptr)
                return Result<CookedPrefabDependency>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            auto envelope = Assets::DecodeCookedArtifact(found->artifact, resourceLimits);
            if (envelope.HasError())
                return Result<CookedPrefabDependency>::Failure(envelope.ErrorValue());
            if (envelope.Value().id != node.assetId || envelope.Value().type != node.assetType || envelope.Value().target != target)
                return Result<CookedPrefabDependency>::Failure(MakeError(PrefabErrors::DependencyGraphInvalid));
            return Result<CookedPrefabDependency>::Success({{node.assetId, node.assetType}, ComputeSha256(std::as_bytes(found->artifact))});
        }

        /** @brief Uses the same validated graph to separate flattened source closure from required runtime resources. */
        [[nodiscard]] Result<std::vector<CookedPrefabDependency>> Dependencies(
            const PrefabSourceResolverSnapshot &sources, const Assets::AssetRegistrySnapshot &registry, const Assets::AssetId root,
            const std::span<const PrefabTemplateCookResource> resources, const AssetCookTargetId &target, const PrefabLimitProfile &limits,
            const PrefabTemplateCookOptions &options) {
            const std::vector<PrefabDependencySource> sourceValues(sources.Sources().begin(), sources.Sources().end());
            auto graph = BuildPrefabDependencyGraph(registry, sourceValues, limits);
            if (graph.HasError())
                return Result<std::vector<CookedPrefabDependency>>::Failure(graph.ErrorValue());
            auto closure = graph.Value().DependencyClosure(std::span{&root, 1});
            if (closure.HasError())
                return Result<std::vector<CookedPrefabDependency>>::Failure(closure.ErrorValue());
            std::vector<CookedPrefabDependency> result;
            for (const auto asset : closure.Value()) {
                if (options.cancellation.IsCancellationRequested())
                    return Result<std::vector<CookedPrefabDependency>>::Failure(MakeError(PrefabErrors::Cancelled));
                const auto *node = graph.Value().FindNode(asset);
                if (node == nullptr)
                    return Result<std::vector<CookedPrefabDependency>>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
                if (node->sourceRevision)
                    continue;
                auto dependency = ResourceDependency(*node, resources, target, options.resourceLimits);
                if (dependency.HasError())
                    return Result<std::vector<CookedPrefabDependency>>::Failure(dependency.ErrorValue());
                result.push_back(std::move(dependency).Value());
            }
            if (result.size() != resources.size())
                return Result<std::vector<CookedPrefabDependency>>::Failure(MakeError(PrefabErrors::DependencyGraphInvalid));
            return Result<std::vector<CookedPrefabDependency>>::Success(std::move(result));
        }

        /** @brief Maps one resolver object to a dense slot while retaining original provider payloads and provenance. */
        [[nodiscard]] Result<CookedPrefabEntity> Entity(const ResolvedPrefabObject &object,
                                                        const std::span<const ResolvedPrefabObject> preceding,
                                                        const PrefabSourceResolverSnapshot &sources) {
            CookedPrefabEntity entity;
            if (object.parent) {
                const auto parent = std::ranges::find(preceding, *object.parent, &ResolvedPrefabObject::key);
                if (parent == preceding.end())
                    return Result<CookedPrefabEntity>::Failure(MakeError(PrefabErrors::HierarchyInvalid));
                entity.parent = CookedPrefabEntitySlot{static_cast<std::uint32_t>(parent - preceding.begin())};
            }
            const auto source = std::ranges::find_if(sources.Sources(), [&object](const PrefabDependencySource &value) {
                return value.document.Data().assetId == object.sourcePrefab;
            });
            if (source == sources.Sources().end())
                return Result<CookedPrefabEntity>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            entity.localTransform = object.effectiveLocalTransform;
            entity.provenance = {object.sourcePrefab, object.key.object, source->sourceRevision.contentDigest};
            entity.members.reserve(object.object.components.size() + object.object.behaviors.size());
            for (const auto &component : object.object.components)
                entity.members.emplace_back(component);
            for (const auto &behavior : object.object.behaviors)
                entity.members.emplace_back(behavior);
            return Result<CookedPrefabEntity>::Success(std::move(entity));
        }
    }  // namespace

    /** @copydoc CookPrefabTemplate */
    Result<CookedPrefab> CookPrefabTemplate(const PrefabSourceResolverSnapshot &sources, const Assets::AssetRegistrySnapshot &registry,
                                            const Assets::AssetId root, const std::span<const PrefabTemplateCookResource> resources,
                                            const AssetCookTargetId &target, const PrefabLimitProfile &limits,
                                            const PrefabTemplateCookOptions &options) {
        if (options.cancellation.IsCancellationRequested())
            return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::Cancelled));
        if (!target.IsValid())
            return Result<CookedPrefab>::Failure(MakeError(AssetCookTargetErrors::Invalid));
        if (sources.RegistryRevision() != registry.Revision())
            return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::ResolutionStale));
        if (resources.size() > limits.Policy().maximumReferencedAssets || resources.size() > options.resourceLimits.maximumAssets)
            return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::ReferenceCountExceeded));
        auto candidate = sources.Resolve(root, PrefabInstanceId::Create(1).Value(), limits);
        if (candidate.HasError())
            return Result<CookedPrefab>::Failure(candidate.ErrorValue());
        auto dependencies = Dependencies(sources, registry, root, resources, target, limits, options);
        if (dependencies.HasError())
            return Result<CookedPrefab>::Failure(dependencies.ErrorValue());
        CookedPrefabData data{.assetId = root, .dependencies = std::move(dependencies).Value()};
        const auto objects = candidate.Value().Objects();
        data.entities.reserve(objects.size());
        PrefabExpansionBudget budget(limits);
        for (std::size_t index = 0; index < objects.size(); ++index) {
            if (options.cancellation.IsCancellationRequested())
                return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::Cancelled));
            const auto &object = objects[index];
            if (auto consumed =
                    budget.Consume(1 + index + sources.Sources().size() + object.object.components.size() + object.object.behaviors.size());
                consumed.HasError())
                return Result<CookedPrefab>::Failure(consumed.ErrorValue());
            auto entity = Entity(object, objects.first(index), sources);
            if (entity.HasError())
                return Result<CookedPrefab>::Failure(entity.ErrorValue());
            data.entities.push_back(std::move(entity).Value());
        }
        if (options.cancellation.IsCancellationRequested())
            return Result<CookedPrefab>::Failure(MakeError(PrefabErrors::Cancelled));
        return CookedPrefab::Create(std::move(data), limits);
    }
}  // namespace Horo::Prefab
