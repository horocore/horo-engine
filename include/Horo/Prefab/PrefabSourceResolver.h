#pragma once

/**
 * @file PrefabSourceResolver.h
 * @brief Revision-pinned prefab source expansion and publication fencing.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Prefab/PrefabDependencyGraph.h"

#include <span>
#include <vector>

namespace Horo::Prefab {
    /** @brief Exact immutable source context captured for one root resolution. */
    struct PrefabResolutionRevision final {
        Assets::AssetRegistryRevision registry;         /**< Asset Registry publication used for every lookup. */
        PrefabSourceRevision rootSource;                /**< Root document revision used for expansion. */
        std::vector<PrefabDependencyNode> dependencies; /**< Canonical reachable nodes, including the root; no paths. */
        std::vector<PrefabDependencyEdge> edges;        /**< Canonical reachable semantic edges used by expansion. */

        [[nodiscard]] bool operator==(const PrefabResolutionRevision &) const noexcept = default;
    };

    /** @brief One authored object copied into an effective revision-pinned hierarchy. */
    struct ResolvedPrefabObject final {
        Assets::AssetId sourcePrefab;                  /**< Source asset that owns the authored object. */
        ExpandedPrefabObjectKey key;                   /**< Stable instance-qualified nested object identity. */
        std::optional<ExpandedPrefabObjectKey> parent; /**< Effective parent, when the object is not a placement root. */
        PrefabObjectNode object;                       /**< Owned authored object data; contains no source path. */
        Math::Transform effectiveLocalTransform;       /**< Placement-adjusted transform used by Scene conversion. */

        [[nodiscard]] bool operator==(const ResolvedPrefabObject &) const noexcept = default;
    };

    /** @brief Complete immutable expansion candidate safe to hand to preview or Scene conversion. */
    class EffectivePrefabCandidate final {
    public:
        /** @brief Returns the root asset identity. @return Stable path-independent identity. */
        [[nodiscard]] Assets::AssetId RootAsset() const noexcept;
        /** @brief Returns the exact registry, reachable source and graph revisions used. @return Immutable revision context. */
        [[nodiscard]] const PrefabResolutionRevision &Revision() const noexcept;
        /** @brief Returns the canonical expanded hierarchy. @return Borrowed immutable objects. */
        [[nodiscard]] std::span<const ResolvedPrefabObject> Objects() const noexcept;

    private:
        friend class PrefabSourceResolverSnapshot;
        friend class PrefabExpansionCache;

        EffectivePrefabCandidate(Assets::AssetId rootAsset, PrefabResolutionRevision revision,
                                 std::vector<ResolvedPrefabObject> objects) noexcept;

        Assets::AssetId rootAsset_{};
        PrefabResolutionRevision revision_{};
        std::vector<ResolvedPrefabObject> objects_;
    };

    /** @brief Self-contained immutable prefab documents and dependency graph from one registry revision. */
    class PrefabSourceResolverSnapshot final {
    public:
        /** @brief Returns the pinned Asset Registry revision. @return Exact immutable publication. */
        [[nodiscard]] Assets::AssetRegistryRevision RegistryRevision() const noexcept;
        /** @brief Returns all owned source documents. @return Borrowed immutable sources. */
        [[nodiscard]] std::span<const PrefabDependencySource> Sources() const noexcept;

        /**
         * @brief Captures complete reachable revision evidence without materializing expanded objects.
         * @param rootAsset Stable requested root.
         * @param limits Immutable policy bounding graph inspection and allocation.
         * @return Owned canonical revision evidence or the same typed graph/budget error as resolution.
         */
        [[nodiscard]] Result<PrefabResolutionRevision> CaptureResolutionRevision(Assets::AssetId rootAsset,
                                                                                 const PrefabLimitProfile &limits) const;

        /**
         * @brief Expands one root as a pure bounded transformation over this snapshot.
         * @param rootAsset Stable root prefab identity.
         * @param instance Stable containing instance identity used by every expanded object key.
         * @param limits Captured project policy bounding recursion, object count and work.
         * @param cancellation Cooperative operation token checked between bounded source/object units.
         * @return Complete effective candidate, or a typed availability, cycle, depth or budget error.
         */
        [[nodiscard]] Result<EffectivePrefabCandidate> Resolve(Assets::AssetId rootAsset, PrefabInstanceId instance,
                                                               const PrefabLimitProfile &limits,
                                                               const CancellationToken &cancellation = {}) const;

        /**
         * @brief Fences captured preview/cache evidence against this publication without invalidating unrelated roots.
         * @param rootAsset Captured root identity.
         * @param revision Immutable evidence retained with the candidate or expanded subtree.
         * @param changedAssets Complete asset publication identities since the captured registry revision, including resource content
         * edits.
         * @param limits Captured work policy; failed or over-budget inspection never reports synchronization.
         * @return Success for an unchanged reachable graph, or ResolutionStale/typed budget failure.
         * @note Owner publication boundary only. Notifications must cover every intervening publication; registry metadata alone
         * cannot detect ordinary resource content edits. Retired snapshots and leases remain immutable.
         */
        [[nodiscard]] Result<void> ValidateRevisionPublication(Assets::AssetId rootAsset, const PrefabResolutionRevision &revision,
                                                               std::span<const Assets::AssetId> changedAssets,
                                                               const PrefabLimitProfile &limits) const;

    private:
        /** @brief Captures the canonical reachable graph, charging caller-owned bounded work before allocation. */
        [[nodiscard]] Result<PrefabResolutionRevision> CaptureRevision(Assets::AssetId rootAsset, PrefabExpansionBudget &budget) const;
        friend Result<PrefabSourceResolverSnapshot> BuildPrefabSourceResolverSnapshot(const Assets::AssetRegistrySnapshot &,
                                                                                      std::vector<PrefabDependencySource>,
                                                                                      const PrefabLimitProfile &);

        PrefabSourceResolverSnapshot(PrefabDependencyGraphSnapshot graph, std::vector<PrefabDependencySource> sources) noexcept;

        PrefabDependencyGraphSnapshot graph_;
        std::vector<PrefabDependencySource> sources_;
    };

    /**
     * @brief Captures all prefab source and dependency reads from one immutable Asset Registry revision.
     * @param registry Pinned registry snapshot used by every lookup.
     * @param sources Owned immutable validated source documents and semantic revisions.
     * @param limits Captured project policy bounding snapshot construction.
     * @return Resolver snapshot or a typed dependency consistency error.
     */
    [[nodiscard]] Result<PrefabSourceResolverSnapshot> BuildPrefabSourceResolverSnapshot(const Assets::AssetRegistrySnapshot &registry,
                                                                                         std::vector<PrefabDependencySource> sources,
                                                                                         const PrefabLimitProfile &limits);

    /**
     * @brief Fences a completed worker candidate against the current publication context.
     * @param candidate Completed immutable worker result.
     * @param current Complete current revision evidence at the owner-thread worker publication boundary.
     * @return Success only when the complete captured context matches; otherwise PrefabErrors::ResolutionStale.
     */
    [[nodiscard]] Result<void> ValidatePrefabCandidatePublication(const EffectivePrefabCandidate &candidate,
                                                                  const PrefabResolutionRevision &current);
}  // namespace Horo::Prefab
