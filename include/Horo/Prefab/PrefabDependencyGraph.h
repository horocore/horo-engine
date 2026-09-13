#pragma once

/**
 * @file PrefabDependencyGraph.h
 * @brief Immutable revision-pinned prefab dependency graph and closure queries.
 */

#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Prefab/PrefabDocument.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Prefab {
    /** @brief Semantic role of one direct dependency owned by a prefab source. */
    enum class PrefabDependencyKind : std::uint8_t {
        Resource,
        NestedPrefab,
        VariantParent
    };

    /** @brief One asset captured by a prefab dependency graph snapshot. */
    struct PrefabDependencyNode final {
        Assets::AssetId assetId;                            /**< Stable path-independent asset identity. */
        Assets::AssetTypeId assetType;                      /**< Type copied from the pinned Asset Registry snapshot. */
        std::optional<PrefabSourceRevision> sourceRevision; /**< Present only for supplied prefab source documents. */

        [[nodiscard]] bool operator==(const PrefabDependencyNode &) const noexcept = default;
    };

    /** @brief One canonical direct dependency from a prefab source to an asset. */
    struct PrefabDependencyEdge final {
        Assets::AssetId sourcePrefab;                              /**< Prefab document that owns the dependency. */
        Assets::AssetId targetAsset;                               /**< Referenced prefab or ordinary asset. */
        PrefabDependencyKind kind{PrefabDependencyKind::Resource}; /**< Dependency's semantic role. */

        [[nodiscard]] constexpr auto operator<=>(const PrefabDependencyEdge &) const noexcept = default;
    };

    /** @brief Owned immutable source input consumed while preparing a graph snapshot. */
    struct PrefabDependencySource final {
        PrefabDocument document;             /**< Validated source document owned by the candidate. */
        PrefabSourceRevision sourceRevision; /**< Exact canonical semantic revision of that document. */
    };

    /** @brief Canonically ordered immutable dependency graph tied to one Asset Registry revision. */
    class PrefabDependencyGraphSnapshot final {
    public:
        /** @brief Returns the Asset Registry revision captured during construction. @return Pinned registry revision. */
        [[nodiscard]] Assets::AssetRegistryRevision RegistryRevision() const noexcept;
        /** @brief Returns all graph nodes in ascending AssetId order. @return Borrowed immutable nodes. */
        [[nodiscard]] std::span<const PrefabDependencyNode> Nodes() const noexcept;
        /** @brief Returns all graph edges in source, target and kind order. @return Borrowed immutable edges. */
        [[nodiscard]] std::span<const PrefabDependencyEdge> Edges() const noexcept;
        /** @brief Finds one captured node. @param assetId Stable asset identity. @return Borrowed node or null. */
        [[nodiscard]] const PrefabDependencyNode *FindNode(Assets::AssetId assetId) const noexcept;
        /** @brief Returns one prefab's contiguous direct dependency range. @param prefabId Source prefab identity.
         * @return Borrowed canonical edges, or an empty span when the prefab has none or is unknown. */
        [[nodiscard]] std::span<const PrefabDependencyEdge> DirectDependencies(Assets::AssetId prefabId) const noexcept;

        /**
         * @brief Computes the unique transitive dependencies of one or more captured roots.
         * @param roots Root asset identities; roots themselves are excluded from the result.
         * @return Ascending AssetId closure, or DependencyUnavailable when a root is not captured.
         */
        [[nodiscard]] Result<std::vector<Assets::AssetId>> DependencyClosure(std::span<const Assets::AssetId> roots) const;

        /**
         * @brief Computes prefab sources invalidated by changed captured assets.
         * @param changedAssets Changed asset identities; unknown assets have no effect.
         * @return Ascending prefab identities including changed prefab sources and every transitive dependent.
         */
        [[nodiscard]] std::vector<Assets::AssetId> InvalidatedPrefabs(std::span<const Assets::AssetId> changedAssets) const;

    private:
        friend Result<PrefabDependencyGraphSnapshot> BuildPrefabDependencyGraph(const Assets::AssetRegistrySnapshot &,
                                                                                const std::vector<PrefabDependencySource> &,
                                                                                const PrefabLimitProfile &);

        PrefabDependencyGraphSnapshot(Assets::AssetRegistryRevision registryRevision, std::vector<PrefabDependencyNode> nodes,
                                      std::vector<PrefabDependencyEdge> edges, std::vector<PrefabDependencyEdge> reverseEdges) noexcept;

        Assets::AssetRegistryRevision registryRevision_{};
        std::vector<PrefabDependencyNode> nodes_;
        std::vector<PrefabDependencyEdge> edges_;
        std::vector<PrefabDependencyEdge> reverseEdges_;
    };

    /**
     * @brief Builds one self-contained dependency snapshot from coherent immutable inputs.
     * @param registry Pinned Asset Registry snapshot used for every identity and type lookup.
     * @param sources Borrowed validated prefab documents and their exact semantic revisions.
     * @param limits Captured project policy whose operation budget bounds graph construction.
     * @return Canonical immutable graph, or a typed consistency, availability, type, revision or budget error.
     */
    [[nodiscard]] Result<PrefabDependencyGraphSnapshot> BuildPrefabDependencyGraph(const Assets::AssetRegistrySnapshot &registry,
                                                                                   const std::vector<PrefabDependencySource> &sources,
                                                                                   const PrefabLimitProfile &limits);
}  // namespace Horo::Prefab
