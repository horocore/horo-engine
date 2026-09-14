#pragma once

/**
 * @file PrefabAssetDependencyClosure.h
 * @brief Canonical revision-pinned prefab asset dependency closure and conflict policy.
 */

#include "Horo/Assets/AssetDependency.h"
#include "Horo/Prefab/PrefabDependencyGraph.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Prefab {
    /** @brief Policy used when the same asset identity carries incompatible captured evidence. */
    enum class PrefabDependencyConflictPolicy : std::uint8_t {
        Reject,
        Count,
    };

    /** @brief One canonical build-time asset requirement with optional prefab-source revision evidence. */
    struct PrefabAssetDependency final {
        Assets::AssetId assetId;                            /**< Stable path-independent identity. */
        Assets::AssetTypeId expectedType;                   /**< Exact type from the pinned registry snapshot. */
        std::optional<PrefabSourceRevision> sourceRevision; /**< Present for captured prefab source dependencies. */

        [[nodiscard]] bool operator==(const PrefabAssetDependency &) const noexcept = default;
    };

    /** @brief Owned immutable dependency closure tied to one Asset Registry publication. */
    class PrefabAssetDependencyClosure final {
    public:
        /** @brief Returns the registry publication used for every requirement. @return Pinned registry revision. */
        [[nodiscard]] Assets::AssetRegistryRevision RegistryRevision() const noexcept;
        /** @brief Returns canonical unique requirements in ascending AssetId order. @return Borrowed immutable requirements. */
        [[nodiscard]] std::span<const PrefabAssetDependency> Dependencies() const noexcept;
        /**
         * @brief Returns source-free dependencies suitable for RuntimeSceneDefinition construction.
         * @return Borrowed canonical requirements excluding captured prefab-source evidence.
         */
        [[nodiscard]] std::span<const Assets::AssetDependency> RuntimeDependencies() const noexcept;

    private:
        friend Result<PrefabAssetDependencyClosure> BuildPrefabAssetDependencyClosure(const Assets::AssetRegistrySnapshot &,
                                                                                      const PrefabDependencyGraphSnapshot &,
                                                                                      std::span<const Assets::AssetId>,
                                                                                      std::span<const PrefabAssetDependency>, std::size_t,
                                                                                      PrefabDependencyConflictPolicy);

        /**
         * @brief Creates an owned closure after validation and canonicalization.
         * @param registryRevision Registry publication that owns the dependency evidence.
         * @param dependencies Canonical unique requirements in ascending AssetId order.
         * @param runtimeDependencies Source-free requirements suitable for runtime scene construction.
         */
        PrefabAssetDependencyClosure(Assets::AssetRegistryRevision registryRevision, std::vector<PrefabAssetDependency> dependencies,
                                     std::vector<Assets::AssetDependency> runtimeDependencies) noexcept;

        Assets::AssetRegistryRevision registryRevision_{};
        std::vector<PrefabAssetDependency> dependencies_;
        std::vector<Assets::AssetDependency> runtimeDependencies_;
    };

    /**
     * @brief Builds one complete canonical closure and strictly merges caller-owned requirements.
     * @param registry Immutable registry snapshot used to verify all existing requirements.
     * @param graph Immutable prefab dependency graph and source-revision evidence from the same registry publication.
     * @param roots Captured prefab roots; roots themselves are excluded from the resulting dependency set.
     * @param existing Existing scene/cook requirements to merge with the prefab closure.
     * @param maximumDependencies Positive hard ceiling applied after identical requirements are deduplicated.
     * @param conflictPolicy Conflict behavior; V1 supports explicit rejection only.
     * @return Owned closure or a typed invalid, unavailable, conflict, capacity or unsupported-policy error.
     * @post Failure publishes no partial closure and never mutates graph or caller-owned inputs.
     */
    [[nodiscard]] Result<PrefabAssetDependencyClosure> BuildPrefabAssetDependencyClosure(
        const Assets::AssetRegistrySnapshot &registry, const PrefabDependencyGraphSnapshot &graph, std::span<const Assets::AssetId> roots,
        std::span<const PrefabAssetDependency> existing, std::size_t maximumDependencies,
        PrefabDependencyConflictPolicy conflictPolicy = PrefabDependencyConflictPolicy::Reject);
}  // namespace Horo::Prefab
