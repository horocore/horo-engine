#pragma once

/**
 * @file StructuralGraphCook.h
 * @brief Bounded, deterministic structural connectivity cook over canonical chunk meshes.
 */

#include "Horo/Destruction/ChunkMeshCook.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace Horo::Destruction {
    inline constexpr std::uint32_t StructuralGraphSchemaVersion = 1;

    struct StructuralPolicyRevisionTag;
    struct StructuralGraphOwnerRevisionTag;
    /** @brief Nonzero published structural-policy revision bound to a graph cook. */
    using StructuralPolicyRevision = DestructionStableIdentity<StructuralPolicyRevisionTag>;
    /** @brief Non-wrapping generation of one graph publication owner. */
    using StructuralGraphOwnerRevision = DestructionStableIdentity<StructuralGraphOwnerRevisionTag>;

    namespace StructuralGraphErrors {
        extern const ErrorCodeDescriptor InvalidInput;
        extern const ErrorCodeDescriptor InvalidIndex;
        extern const ErrorCodeDescriptor UnstableOrder;
        extern const ErrorCodeDescriptor HierarchyCycle;
        extern const ErrorCodeDescriptor DisconnectedRequired;
        extern const ErrorCodeDescriptor LimitExceeded;
        extern const ErrorCodeDescriptor Unsupported;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor Shutdown;
    }  // namespace StructuralGraphErrors

    /** @brief Authored structural intent for the chunk at the same canonical mesh-table index. */
    struct StructuralChunkInput final {
        DestructionChunkId id{};     /**< Must equal the stable ID at this mesh-table index. */
        DestructionChunkId parent{}; /**< Optional hierarchy parent; zero denotes a root. */
        bool anchor{};               /**< Immutable support root. */
        bool required{};             /**< Must be reachable from an anchor. */
    };

    /** @brief Canonically ordered undirected contact, with a positive finite support weight. */
    struct StructuralContactInput final {
        std::uint32_t low{};  /**< Lower canonical chunk-table index. */
        std::uint32_t high{}; /**< Higher canonical chunk-table index. */
        double weight{};      /**< Authored or measured contact support, in deterministic source units. */
    };

    /** @brief Exact mesh and policy revision captured before detached work. */
    struct StructuralGraphCookRequest final {
        FractureArtifactContentIdentity content{};
        Sha256Digest meshIntegrityDigest{};
        StructuralGraphOwnerRevision ownerRevision{};
        StructuralPolicyRevision policyRevision{};
        DestructionFeatureSet requiredFeatures{};
        DestructionLimits limits{};
        std::vector<StructuralChunkInput> chunks;
        std::vector<StructuralContactInput> contacts;
    };

    /** @brief Structural classification, independent of runtime breakage. */
    struct StructuralChunkFlags final {
        bool anchor{};
        bool required{};
        bool initiallySupported{};
        bool hasParent{};
    };

    /** @brief One canonical chunk and its stable-order neighbor references. */
    struct StructuralGraphChunk final {
        DestructionChunkId id{};
        DestructionChunkId parent{};
        StructuralChunkFlags flags{};
        std::uint32_t island{};               /**< Zero-based component index in smallest-ID order. */
        double supportWeight{};               /**< Sum of incident canonical contact weights. */
        std::vector<std::uint32_t> adjacency; /**< Neighbor indices in stable-ID order. */
    };

    /** @brief Finite validation facts included in the immutable graph digest. */
    struct StructuralGraphValidation final {
        std::uint32_t chunkCount{};
        std::uint32_t contactCount{};
        std::uint32_t islandCount{};
        std::uint32_t anchorCount{};
        std::uint32_t requiredCount{};
        std::uint32_t maximumHierarchyDepth{};
        std::uint64_t workItems{};
        std::uint64_t estimatedBytes{};
    };

    /** @brief Detached solver-neutral structural graph; only const shared snapshots are published. */
    struct StructuralGraphArtifact final {
        FractureArtifactContentIdentity content{};
        Sha256Digest meshIntegrityDigest{};
        StructuralGraphOwnerRevision ownerRevision{};
        StructuralPolicyRevision policyRevision{};
        Sha256Digest integrityDigest{};
        DestructionFeatureTier tier{};
        DestructionFeatureSet producedFeatures{};
        std::uint32_t schemaVersion{StructuralGraphSchemaVersion};
        StructuralGraphValidation validation{};
        std::vector<StructuralGraphChunk> chunks;
        std::vector<StructuralContactInput> contacts;
    };

    /**
     * @brief Cooks canonical connectivity, support and islands without publishing partial output.
     * @param mesh Exact immutable canonical chunk mesh artifact.
     * @param request Captured ordered topology, identities, policy and finite limits.
     * @param cancellation Cooperative cancellation checked throughout graph traversal.
     * @return Detached graph or typed failure with contextual diagnostics.
     */
    [[nodiscard]] Result<std::shared_ptr<const StructuralGraphArtifact>> CookStructuralGraph(const ChunkMeshArtifact &mesh,
                                                                                             const StructuralGraphCookRequest &request,
                                                                                             const CancellationToken &cancellation);

    /** @brief Single-thread publication owner with cancellation and non-wrapping revision fences. */
    class StructuralGraphCookOwner final {
    public:
        StructuralGraphCookOwner() = default;
        StructuralGraphCookOwner(const StructuralGraphCookOwner &) = delete;
        StructuralGraphCookOwner &operator=(const StructuralGraphCookOwner &) = delete;

        /** @brief Returns the current owner revision. @return Nonzero revision. */
        [[nodiscard]] StructuralGraphOwnerRevision Revision() const noexcept;
        /** @brief Returns the current detached-work cancellation token. @return Current token. */
        [[nodiscard]] CancellationToken Token() const noexcept;
        /** @brief Returns the last published immutable graph. @return Shared snapshot or null. */
        [[nodiscard]] std::shared_ptr<const StructuralGraphArtifact> Snapshot() const noexcept;
        /**
         * @brief Accepts only a complete graph for the exact owner, content and mesh generation.
         * @param candidate Detached immutable graph.
         * @param expectedRevision Owner revision captured before cooking.
         * @param currentContent Exact current fracture content.
         * @param currentMeshDigest Exact current mesh integrity digest.
         * @param currentPolicyRevision Exact current structural policy revision.
         * @return Success or typed failure leaving the old snapshot intact.
         */
        [[nodiscard]] Result<void> Accept(std::shared_ptr<const StructuralGraphArtifact> candidate,
                                          StructuralGraphOwnerRevision expectedRevision,
                                          const FractureArtifactContentIdentity &currentContent, const Sha256Digest &currentMeshDigest,
                                          StructuralPolicyRevision currentPolicyRevision);
        /** @brief Cancels pending work and advances the revision. @return Success or typed exhaustion/shutdown failure. */
        [[nodiscard]] Result<void> Invalidate();
        /** @brief Closes admission and cancels work, retaining the last snapshot. */
        void Shutdown() noexcept;

    private:
        std::shared_ptr<const StructuralGraphArtifact> current_;
        CancellationSource cancellation_;
        StructuralGraphOwnerRevision revision_{StructuralGraphOwnerRevision::Create(1).Value()};
        bool shutdown_{};
    };
}  // namespace Horo::Destruction
