#include "Horo/Prefab/PrefabAssetDependencyClosure.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Prefab {
    namespace {
        [[nodiscard]] bool IsValid(const PrefabAssetDependency &dependency) noexcept {
            return dependency.assetId.IsValid() && !dependency.expectedType.Value().empty();
        }

        [[nodiscard]] std::string DependencyContext(const Assets::AssetId assetId, const std::string_view reason,
                                                    const std::optional<Assets::AssetId> sourcePrefab = std::nullopt) {
            const std::string source = sourcePrefab ? "Prefab source " + sourcePrefab->ToString() : "Scene/cook source";
            return source + " dependency member " + assetId.ToString() + ": " + std::string{reason};
        }

        [[nodiscard]] Result<void> MergeDependency(std::vector<PrefabAssetDependency> &dependencies, PrefabAssetDependency dependency,
                                                   const std::size_t maximumDependencies,
                                                   const std::optional<Assets::AssetId> sourcePrefab = std::nullopt) {
            if (!IsValid(dependency))
                return Result<void>::Failure(MakeError(PrefabErrors::ReferenceInvalid));
            const auto position = std::ranges::lower_bound(dependencies, dependency.assetId, {}, &PrefabAssetDependency::assetId);
            if (position != dependencies.end() && position->assetId == dependency.assetId) {
                if (*position != dependency)
                    return Result<void>::Failure(
                        MakeError(PrefabErrors::DependencyConflict,
                                  DependencyContext(dependency.assetId, "captured evidence conflicts.", sourcePrefab)));
                return Result<void>::Success();
            }
            if (dependencies.size() >= maximumDependencies)
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyClosureCapacityExceeded));
            dependencies.insert(position, std::move(dependency));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateExisting(const Assets::AssetRegistrySnapshot &registry,
                                                    const PrefabDependencyGraphSnapshot &graph, const PrefabAssetDependency &dependency) {
            if (!IsValid(dependency))
                return Result<void>::Failure(MakeError(PrefabErrors::ReferenceInvalid));
            const Assets::AssetRecord *record = registry.Find(dependency.assetId);
            if (record == nullptr)
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyUnavailable,
                                                       DependencyContext(dependency.assetId, "the pinned registry has no record.")));
            if (record->type != dependency.expectedType)
                return Result<void>::Failure(
                    MakeError(PrefabErrors::DependencyConflict,
                              DependencyContext(dependency.assetId, "the expected type conflicts with the registry.")));
            if (!dependency.sourceRevision)
                return Result<void>::Success();
            if (const PrefabDependencyNode *node = graph.FindNode(dependency.assetId);
                node == nullptr || node->sourceRevision != dependency.sourceRevision)
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyConflict,
                                                       DependencyContext(dependency.assetId, "the prefab source revision conflicts.")));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRequest(const Assets::AssetRegistrySnapshot &registry,
                                                   const PrefabDependencyGraphSnapshot &graph, const std::span<const Assets::AssetId> roots,
                                                   const std::size_t maximumDependencies,
                                                   const PrefabDependencyConflictPolicy conflictPolicy) {
            if (roots.empty() || std::ranges::any_of(roots, [](const Assets::AssetId root) {
                return !root.IsValid();
            }))
                return Result<void>::Failure(MakeError(PrefabErrors::ReferenceInvalid));
            if (maximumDependencies == 0)
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyClosureCapacityExceeded));
            if (conflictPolicy != PrefabDependencyConflictPolicy::Reject)
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyConflictPolicyUnsupported));
            if (registry.Revision() != graph.RegistryRevision())
                return Result<void>::Failure(MakeError(PrefabErrors::ResolutionStale));
            for (const Assets::AssetId root : roots) {
                if (graph.FindNode(root) == nullptr)
                    return Result<void>::Failure(
                        MakeError(PrefabErrors::DependencyUnavailable, DependencyContext(root, "the captured root is unavailable.", root)));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MergeExistingDependencies(const Assets::AssetRegistrySnapshot &registry,
                                                             const PrefabDependencyGraphSnapshot &graph,
                                                             const std::span<const PrefabAssetDependency> existing,
                                                             const std::size_t maximumDependencies,
                                                             std::vector<PrefabAssetDependency> &dependencies) {
            for (const PrefabAssetDependency &dependency : existing) {
                if (const auto valid = ValidateExisting(registry, graph, dependency); valid.HasError())
                    return valid;
                if (const auto merged = MergeDependency(dependencies, dependency, maximumDependencies); merged.HasError())
                    return merged;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MergeGraphDependencies(const PrefabDependencyGraphSnapshot &graph,
                                                          const std::span<const Assets::AssetId> roots,
                                                          const std::size_t maximumDependencies,
                                                          std::vector<PrefabAssetDependency> &dependencies) {
            auto closure = graph.DependencyClosure(roots);
            if (closure.HasError())
                return Result<void>::Failure(closure.ErrorValue());
            for (const Assets::AssetId assetId : closure.Value()) {
                const PrefabDependencyNode *node = graph.FindNode(assetId);
                if (node == nullptr)
                    return Result<void>::Failure(MakeError(PrefabErrors::DependencyUnavailable,
                                                           DependencyContext(assetId, "the captured graph node is unavailable.")));
                if (const auto merged = MergeDependency(dependencies, {node->assetId, node->assetType, node->sourceRevision},
                                                        maximumDependencies, roots.front());
                    merged.HasError())
                    return merged;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PrefabAssetDependencyClosure::PrefabAssetDependencyClosure */
    PrefabAssetDependencyClosure::PrefabAssetDependencyClosure(const Assets::AssetRegistryRevision registryRevision,
                                                               std::vector<PrefabAssetDependency> dependencies,
                                                               std::vector<Assets::AssetDependency> runtimeDependencies) noexcept
        : registryRevision_(registryRevision), dependencies_(std::move(dependencies)),
          runtimeDependencies_(std::move(runtimeDependencies)) {}

    /** @copydoc PrefabAssetDependencyClosure::RegistryRevision */
    Assets::AssetRegistryRevision PrefabAssetDependencyClosure::RegistryRevision() const noexcept {
        return registryRevision_;
    }

    /** @copydoc PrefabAssetDependencyClosure::Dependencies */
    std::span<const PrefabAssetDependency> PrefabAssetDependencyClosure::Dependencies() const noexcept {
        return dependencies_;
    }

    /** @copydoc PrefabAssetDependencyClosure::RuntimeDependencies */
    std::span<const Assets::AssetDependency> PrefabAssetDependencyClosure::RuntimeDependencies() const noexcept {
        return runtimeDependencies_;
    }

    /** @copydoc BuildPrefabAssetDependencyClosure */
    Result<PrefabAssetDependencyClosure> BuildPrefabAssetDependencyClosure(const Assets::AssetRegistrySnapshot &registry,
                                                                           const PrefabDependencyGraphSnapshot &graph,
                                                                           const std::span<const Assets::AssetId> roots,
                                                                           const std::span<const PrefabAssetDependency> existing,
                                                                           const std::size_t maximumDependencies,
                                                                           const PrefabDependencyConflictPolicy conflictPolicy) {
        if (const auto valid = ValidateRequest(registry, graph, roots, maximumDependencies, conflictPolicy); valid.HasError())
            return Result<PrefabAssetDependencyClosure>::Failure(valid.ErrorValue());

        std::vector<PrefabAssetDependency> dependencies;
        const std::size_t graphCapacity = std::min(maximumDependencies, graph.Nodes().size());
        const std::size_t existingCapacity = std::min(maximumDependencies - graphCapacity, existing.size());
        dependencies.reserve(graphCapacity + existingCapacity);
        if (const auto merged = MergeExistingDependencies(registry, graph, existing, maximumDependencies, dependencies); merged.HasError())
            return Result<PrefabAssetDependencyClosure>::Failure(merged.ErrorValue());
        if (const auto merged = MergeGraphDependencies(graph, roots, maximumDependencies, dependencies); merged.HasError())
            return Result<PrefabAssetDependencyClosure>::Failure(merged.ErrorValue());

        std::vector<Assets::AssetDependency> runtimeDependencies;
        runtimeDependencies.reserve(dependencies.size());
        for (const PrefabAssetDependency &dependency : dependencies) {
            if (!dependency.sourceRevision)
                runtimeDependencies.emplace_back(dependency.assetId, dependency.expectedType);
        }
        return Result<PrefabAssetDependencyClosure>::Success(
            PrefabAssetDependencyClosure{graph.RegistryRevision(), std::move(dependencies), std::move(runtimeDependencies)});
    }
}  // namespace Horo::Prefab
