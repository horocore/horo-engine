#pragma once

/**
 * @file PrefabSourceResolver.h
 * @brief Revision-pinned prefab source expansion and publication fencing.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Prefab/PrefabCompositionOrigin.h"
#include "Horo/Prefab/PrefabDependencyGraph.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Prefab {
    class PrefabValueSchemaSnapshot;

    /** @brief Resolver-admitted evidence computed from the actual canonical bytes of one owned validated source. */
    struct PrefabCanonicalSourceCommitment final {
        Assets::AssetId asset;      /**< Exact source identity, never an input-vector position. */
        Sha256Digest digest;        /**< Digest of SerializeCanonical bytes, independent of the caller's claimed revision. */
        std::size_t encodedBytes{}; /**< Exact canonical wire length for bounded cache/admission accounting. */
        [[nodiscard]] bool operator==(const PrefabCanonicalSourceCommitment &) const noexcept = default;
    };

    /** @brief Exact immutable source context captured for one root resolution. */
    struct PrefabResolutionRevision final {
        Assets::AssetRegistryRevision registry;                        /**< Asset Registry publication used for every lookup. */
        PrefabSourceRevision rootSource;                               /**< Root document revision used for expansion. */
        std::vector<PrefabDependencyNode> dependencies;                /**< Canonical reachable nodes, including the root; no paths. */
        std::vector<PrefabDependencyEdge> edges;                       /**< Canonical reachable semantic edges used by expansion. */
        std::vector<PrefabCanonicalSourceCommitment> canonicalSources; /**< Actual canonical bytes, independent of claimed revisions. */

        PrefabSourceRevisionPolicy sourceRevisionPolicy{PrefabSourceRevisionPolicy::RequireExact};
        std::vector<PrefabDependencyRevisionSkew>
            revisionSkews;                  /**< Actual target and original authoring evidence; never runtime permission. */
        Sha256Digest valueSchemaRevision{}; /**< Exact immutable validating schema captured with this source resolver. */
        [[nodiscard]] bool operator==(const PrefabResolutionRevision &) const noexcept = default;
    };

    /** @brief One authored object copied into an effective revision-pinned hierarchy. */
    struct ResolvedPrefabObject final {
        Assets::AssetId sourcePrefab;                  /**< Source asset that owns the authored object. */
        ExpandedPrefabObjectKey key;                   /**< Stable instance-qualified nested object identity. */
        std::optional<ExpandedPrefabObjectKey> parent; /**< Effective parent, when the object is not a placement root. */
        PrefabObjectNode object;                       /**< Owned authored object data; contains no source path. */
        Math::Transform effectiveLocalTransform;       /**< Placement-adjusted transform used by Scene conversion. */
        Math::Transform placementTransform; /**< Exact mount TRS applied to a nested root; identity for ordinary source objects. */
        std::vector<PrefabCompositionOriginStep> originChain; /**< Bounded actual application lineage, independent of paths/labels. */

        [[nodiscard]] bool operator==(const ResolvedPrefabObject &) const noexcept = default;
    };

    /** @brief Complete immutable expansion candidate safe to hand to preview or Scene conversion. */
    class EffectivePrefabCandidate final {
    public:
        /** @brief Returns the root asset identity. @return Stable path-independent identity. */
        [[nodiscard]] Assets::AssetId RootAsset() const noexcept;
        /** @brief Returns the exact registry, reachable source and graph revisions used. @return Immutable revision context. */
        [[nodiscard]] const PrefabResolutionRevision &Revision() const noexcept;

        /** @brief Returns owned numeric observation handles, excluded from semantic identity. @return Immutable configuration. */
        [[nodiscard]] const PrefabMetricHandles &Metrics() const noexcept {
            return metrics_;
        }

        /** @brief Returns the canonical expanded hierarchy. @return Borrowed immutable objects. */
        [[nodiscard]] std::span<const ResolvedPrefabObject> Objects() const noexcept;
        /** @brief Read exact instance value-intent commitment. @return Zero for unmodified source expansion. */
        [[nodiscard]] Sha256Digest ValueOverridesDigest() const noexcept;

    private:
        friend class PrefabValueOverrideResolver;
        friend class PrefabSourceLayerResolver;
        friend class PrefabSourceResolverSnapshot;
        friend class PrefabExpansionCache;

        EffectivePrefabCandidate(Assets::AssetId rootAsset, PrefabResolutionRevision revision, std::vector<ResolvedPrefabObject> objects,
                                 const Sha256Digest &valueOverrides = {}, const PrefabMetricHandles &metrics = {}) noexcept;

        Assets::AssetId rootAsset_{};
        PrefabResolutionRevision revision_{};
        std::vector<ResolvedPrefabObject> objects_;
        Sha256Digest valueOverrides_{};
        PrefabMetricHandles metrics_;
    };

    /** @brief Owned result for one actual source-document layer; no partial effective hierarchy is exposed. */
    struct PrefabLayerOverrideEvidence final {
        Assets::AssetId owner;
        std::optional<LocalObjectId> placement;              /**< Absent identifies the owner's variant layer. */
        std::vector<PrefabCompositionOriginStep> occurrence; /**< Exact semantic import path, never serialized array positions. */
        PrefabValueOverrideSet rebased;
    };

    /** @brief Authoring result from the same strict resolver; unresolved intent prevents effective publication. */
    struct PrefabSourceOverrideResolution final {
        std::optional<EffectivePrefabCandidate> effective;
        std::vector<PrefabLayerOverrideEvidence>
            layers; /**< Canonical inspected prefix; downstream layers remain intact in Sources() when their input is unresolved. */
        PrefabResolutionRevision revision; /**< Complete policy/schema/actual+authored source capture for the document acceptance fence. */
    };

    /** @brief Self-contained immutable prefab documents and dependency graph from one registry revision. */
    class PrefabSourceResolverSnapshot final {
    public:
        /** @brief Returns the pinned Asset Registry revision. @return Exact immutable publication. */
        [[nodiscard]] Assets::AssetRegistryRevision RegistryRevision() const noexcept;

        /** @brief Returns explicitly captured numeric observation handles. @return Owned configuration with no host borrow. */
        [[nodiscard]] const PrefabMetricHandles &Metrics() const noexcept {
            return metrics_;
        }

        /** @brief Borrows the exact graph already validated by this resolver; consumers never rebuild a competing capture. */
        [[nodiscard]] const PrefabDependencyGraphSnapshot &DependencyGraph() const noexcept;

        /** @brief Returns all owned source documents. @return Borrowed immutable sources. */
        [[nodiscard]] std::span<const PrefabDependencySource> Sources() const noexcept;
        /**
         * @brief Returns complete immutable canonical evidence captured before this resolver was published.
         * @return Borrowed commitments in unique ascending AssetId order; lifetime is this snapshot's.
         * @details Copying or retiring source snapshots cannot mutate these values. Cache capture and
         * completion validation reuse this evidence without re-encoding JSON or trusting claimed digests.
         */
        [[nodiscard]] std::span<const PrefabCanonicalSourceCommitment> CanonicalSourceCommitments() const noexcept;

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

        /** @brief Resolves exactly once while retaining canonical source-layer conflicts/orphans for document transactions.
         * @param rootAsset Exact source asset. @param instance Exact containing occurrence.
         * @param limits Captured shared work policy. @param cancellation Cooperative operation cancellation.
         * @return Complete candidate or bounded owned unresolved layer evidence; never a partial effective hierarchy. */
        [[nodiscard]] Result<PrefabSourceOverrideResolution> ResolveOverrides(Assets::AssetId rootAsset, PrefabInstanceId instance,
                                                                              const PrefabLimitProfile &limits,
                                                                              const CancellationToken &cancellation = {}) const;
        /** @brief Returns the exact copied schema authority digest, or zero when no schema policy was supplied. */
        [[nodiscard]] Sha256Digest ValueSchemaRevision() const noexcept;
        /** @brief Borrows immutable validation metadata owned by this exact resolver publication. */
        [[nodiscard]] const PrefabValueSchemaSnapshot *ValueSchemas() const noexcept;

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
        /** @brief Validates and constructs only operation-owned complete layer evidence inside the allocation boundary. */
        [[nodiscard]] Result<PrefabSourceOverrideResolution> ResolveOverridesImpl(Assets::AssetId root, PrefabInstanceId instance,
                                                                                  const PrefabLimitProfile &limits,
                                                                                  const CancellationToken &cancellation) const;
        /** @brief Captures the canonical reachable graph, charging caller-owned bounded work before allocation. */
        [[nodiscard]] Result<PrefabResolutionRevision> CaptureRevision(Assets::AssetId rootAsset, PrefabExpansionBudget &budget) const;
        friend Result<PrefabSourceResolverSnapshot> BuildPrefabSourceResolverSnapshot(
            const Assets::AssetRegistrySnapshot &, std::vector<PrefabDependencySource>, const PrefabLimitProfile &,
            const PrefabMetricHandles &, std::shared_ptr<const PrefabValueSchemaSnapshot>, PrefabSourceRevisionPolicy);

        PrefabSourceResolverSnapshot(PrefabDependencyGraphSnapshot graph, std::vector<PrefabDependencySource> sources,
                                     std::vector<PrefabCanonicalSourceCommitment> commitments, const PrefabMetricHandles &metrics,
                                     std::shared_ptr<const PrefabValueSchemaSnapshot> schemas) noexcept;

        PrefabDependencyGraphSnapshot graph_;
        std::vector<PrefabDependencySource> sources_;
        std::vector<PrefabCanonicalSourceCommitment> commitments_;
        PrefabMetricHandles metrics_;
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas_;
    };

    /**
     * @brief Captures all prefab source and dependency reads from one immutable Asset Registry revision.
     * @param registry Pinned registry snapshot used by every lookup.
     * @param sources Owned immutable validated source documents and semantic revisions.
     * @param limits Captured project policy bounding snapshot construction.
     * @param metrics Explicit host-prebound handles retained by value, excluded from semantic commitments.
     * @return Resolver snapshot or a typed dependency consistency, source encoding or allocation error.
     * @details Admission computes exact canonical digest/length evidence from each owned validated document;
     * claimed source revisions remain separate graph evidence and do not supply those commitments.
     */
    [[nodiscard]] Result<PrefabSourceResolverSnapshot> BuildPrefabSourceResolverSnapshot(
        const Assets::AssetRegistrySnapshot &registry, std::vector<PrefabDependencySource> sources, const PrefabLimitProfile &limits,
        const PrefabMetricHandles &metrics = {}, std::shared_ptr<const PrefabValueSchemaSnapshot> schemas = {},
        PrefabSourceRevisionPolicy revisionPolicy = PrefabSourceRevisionPolicy::RequireExact);

    /**
     * @brief Fences a completed worker candidate against the current publication context.
     * @param candidate Completed immutable worker result.
     * @param current Complete current revision evidence at the owner-thread worker publication boundary.
     * @return Success only when the complete captured context matches; otherwise PrefabErrors::ResolutionStale.
     */
    [[nodiscard]] Result<void> ValidatePrefabCandidatePublication(const EffectivePrefabCandidate &candidate,
                                                                  const PrefabResolutionRevision &current);
}  // namespace Horo::Prefab
