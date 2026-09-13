#pragma once

/**
 * @file PrefabSourceResolver.h
 * @brief Revision-pinned prefab source expansion and publication fencing.
 */

#include "Horo/Prefab/PrefabDependencyGraph.h"

#include <span>
#include <vector>

namespace Horo::Prefab {
    /** @brief Exact immutable source context captured for one root resolution. */
    struct PrefabResolutionRevision final {
        Assets::AssetRegistryRevision registry; /**< Asset Registry publication used for every lookup. */
        PrefabSourceRevision rootSource;        /**< Root document revision used for expansion. */

        [[nodiscard]] bool operator==(const PrefabResolutionRevision &) const noexcept = default;
    };

    /** @brief One authored object copied into an effective revision-pinned hierarchy. */
    struct ResolvedPrefabObject final {
        Assets::AssetId sourcePrefab; /**< Source asset that owns the authored object. */
        ExpandedPrefabObjectKey key;  /**< Stable instance-qualified nested object identity. */
        PrefabObjectNode object;      /**< Owned portable object data; contains no source path. */

        [[nodiscard]] bool operator==(const ResolvedPrefabObject &) const noexcept = default;
    };

    /** @brief Complete immutable expansion candidate safe to hand to preview or Scene conversion. */
    class EffectivePrefabCandidate final {
    public:
        /** @brief Returns the root asset identity. @return Stable path-independent identity. */
        [[nodiscard]] Assets::AssetId RootAsset() const noexcept;
        /** @brief Returns the exact registry and root-source revisions used. @return Immutable revision context. */
        [[nodiscard]] const PrefabResolutionRevision &Revision() const noexcept;
        /** @brief Returns the canonical expanded hierarchy. @return Borrowed immutable objects. */
        [[nodiscard]] std::span<const ResolvedPrefabObject> Objects() const noexcept;

    private:
        friend class PrefabSourceResolverSnapshot;

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
         * @brief Expands one root as a pure bounded transformation over this snapshot.
         * @param rootAsset Stable root prefab identity.
         * @param instance Stable containing instance identity used by every expanded object key.
         * @param limits Captured project policy bounding recursion, object count and work.
         * @return Complete effective candidate, or a typed availability, cycle, depth or budget error.
         */
        [[nodiscard]] Result<EffectivePrefabCandidate> Resolve(Assets::AssetId rootAsset, PrefabInstanceId instance,
                                                               const PrefabLimitProfile &limits) const;

    private:
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
     * @param current Current registry and root-document revisions at the owner-thread publication boundary.
     * @return Success only when both captured revisions still match; otherwise PrefabErrors::ResolutionStale.
     */
    [[nodiscard]] Result<void> ValidatePrefabCandidatePublication(const EffectivePrefabCandidate &candidate,
                                                                  const PrefabResolutionRevision &current);
}  // namespace Horo::Prefab
