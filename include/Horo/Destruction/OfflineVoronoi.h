#pragma once

/**
 * @file OfflineVoronoi.h
 * @brief Bounded, deterministic, detached offline Voronoi fracture generation.
 */

#include "Horo/Destruction/DestructibleDescriptor.h"
#include "Horo/Foundation/CancellationToken.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Horo::Destruction {
    /** @brief Exact algorithm and output schema used by this offline cook. */
    inline constexpr std::uint32_t OfflineVoronoiSchemaVersion = 1;

    namespace OfflineVoronoiErrors {
        extern const ErrorCodeDescriptor InvalidInput;  /**< Source, recipe, or provenance is malformed. */
        extern const ErrorCodeDescriptor InvalidMesh;   /**< Source is not a closed, consistently wound mesh. */
        extern const ErrorCodeDescriptor InvalidSites;  /**< Requested sites cannot form nonempty distinct cells. */
        extern const ErrorCodeDescriptor LimitExceeded; /**< Explicit geometry, memory, or work budget would be exceeded. */
        extern const ErrorCodeDescriptor Cancelled;     /**< Generation stopped before candidate completion. */
        extern const ErrorCodeDescriptor Stale;         /**< Source or recipe revision changed before acceptance. */
        extern const ErrorCodeDescriptor Shutdown;      /**< Candidate owner closed admission. */
    }  // namespace OfflineVoronoiErrors

    /** @brief Immutable-input source snapshot in canonical Horo coordinates. */
    struct OfflineVoronoiSource final {
        Assets::AssetId asset{};  /**< Stable source identity, not a path. */
        std::uint64_t revision{}; /**< Nonzero immutable source revision. */
        Sha256Digest digest{};    /**< Digest of normalized source bytes. */
        std::vector<std::array<float, 3>> positions;
        std::vector<std::uint32_t> indices;       /**< Outward-wound triangles. */
        std::vector<std::uint32_t> materialSlots; /**< One slot per source triangle. */
    };

    /** @brief Hashes normalized source geometry in canonical field order. @param source Source fields excluding digest.
     * @return SHA-256 over versioned geometry, indices, and material slots.
     */
    [[nodiscard]] Sha256Digest ComputeOfflineVoronoiSourceDigest(const OfflineVoronoiSource &source);

    /** @brief Explicit semantic inputs; empty sites use seeded deterministic placement. */
    struct OfflineVoronoiRecipe final {
        std::uint64_t id{};                       /**< Stable nonzero authored recipe identity. */
        std::uint64_t revision{};                 /**< Nonzero authored recipe revision. */
        std::uint64_t seed{};                     /**< Deterministic site-generation seed. */
        std::uint32_t siteCount{};                /**< Exact requested chunk count. */
        std::vector<DestructionChunkId> siteIds;  /**< Stable authored IDs paired with sites, including generated sites. */
        std::vector<std::array<double, 3>> sites; /**< Explicit canonical sites, or empty. */
        std::uint32_t interiorMaterialSlot{};     /**< Interior face material identity. */
        std::uint32_t toolchainVersion{};         /**< Nonzero target-neutral toolchain generation. */
        Sha256Digest toolchainDigest{};           /**< Exact pinned toolchain identity. */
        DestructionFeatureTier tier{};            /**< Exact selected feature tier. */
        DestructionLimits limits{};               /**< Explicit finite product limits. */
        std::uint64_t maximumVertices{};          /**< Total output vertex capacity. */
        std::uint64_t maximumTriangles{};         /**< Total output triangle capacity. */
        std::uint64_t maximumWorkItems{};         /**< Total clipping and validation capacity. */
        std::uint32_t maximumConvexRegions{};     /**< Peak bounded source decomposition region count. */
    };

    /** @brief Canonical triangle with source/interior material semantics. */
    struct OfflineVoronoiTriangle final {
        std::array<std::uint32_t, 3> indices{};
        std::uint32_t materialSlot{};
        bool interior{};
    };

    /** @brief One closed convex solver-neutral input for later Physics shape cooking. */
    struct OfflineVoronoiCollisionPiece final {
        std::vector<std::array<float, 3>> positions;
        std::vector<std::array<std::uint32_t, 3>> triangles;
        double volume{};
    };

    /** @brief One semantic cell, possibly with multiple convex solver-neutral collision pieces. */
    struct OfflineVoronoiChunk final {
        DestructionChunkId id{}; /**< Stable authored site identity, never a table position. */
        std::array<double, 3> site{};
        std::vector<std::array<float, 3>> positions;
        std::vector<OfflineVoronoiTriangle> triangles;
        std::vector<OfflineVoronoiCollisionPiece> collisionPieces;
        std::vector<DestructionChunkId> neighbors; /**< Stable sorted IDs sharing an interior face. */
        double volume{};
        std::array<double, 3> centerOfMass{};
    };

    /** @brief Detached canonical result, never a published asset or runtime shape. */
    struct OfflineVoronoiCandidate final {
        Assets::AssetId sourceAsset{};
        std::uint64_t sourceRevision{};
        Sha256Digest sourceDigest{};
        std::uint64_t recipeId{};
        std::uint64_t recipeRevision{};
        Sha256Digest semanticFingerprint{};
        Sha256Digest toolchainDigest{};
        std::uint32_t schemaVersion{OfflineVoronoiSchemaVersion};
        std::vector<OfflineVoronoiChunk> chunks;
        std::uint64_t estimatedBytes{};
        std::uint64_t workItems{};

    private:
        friend Result<OfflineVoronoiCandidate> GenerateOfflineVoronoi(const OfflineVoronoiSource &, const OfflineVoronoiRecipe &,
                                                                      const CancellationToken &);
        friend class OfflineVoronoiOwner;
        std::uint64_t outputChecksum_{}; /**< Detects accidental candidate mutation before owner acceptance. */
    };

    /**
     * @brief Generates exact, clipped Voronoi cells from a validated closed source in an offline cook.
     * @param source Owned normalized source snapshot; no native or runtime objects are accessed.
     * @param recipe Captured explicit seed, settings, limits, tier, and toolchain identity.
     * @param cancellation Cooperative token checked during validation, placement, and clipping.
     * @return Detached candidate or typed zero-publication failure. Only equal inputs under the same toolchain promise equal bytes.
     */
    [[nodiscard]] Result<OfflineVoronoiCandidate> GenerateOfflineVoronoi(const OfflineVoronoiSource &source,
                                                                         const OfflineVoronoiRecipe &recipe,
                                                                         const CancellationToken &cancellation);

    /** @brief Single-thread authoring acceptance boundary with non-wrapping revision and cancellation fence. */
    class OfflineVoronoiOwner final {
    public:
        /** @brief Starts an empty owner at revision one. */
        OfflineVoronoiOwner() = default;
        OfflineVoronoiOwner(const OfflineVoronoiOwner &) = delete;
        OfflineVoronoiOwner &operator=(const OfflineVoronoiOwner &) = delete;

        /** @brief Returns the current owner revision. @return Nonzero revision. */
        [[nodiscard]] std::uint64_t Revision() const noexcept {
            return revision_;
        }

        /** @brief Returns cancellation for detached work. @return Current generation token. */
        [[nodiscard]] CancellationToken Token() const noexcept {
            return cancellation_.Token();
        }

        /** @brief Returns last accepted immutable candidate. @return Shared snapshot or null. */
        [[nodiscard]] std::shared_ptr<const OfflineVoronoiCandidate> Snapshot() const noexcept {
            return current_;
        }

        /**
         * @brief Accepts a complete candidate only for exact captured owner and input revisions.
         * @param candidate Detached candidate.
         * @param expectedOwnerRevision Owner revision captured before work.
         * @param currentSource Current source snapshot used for stale validation.
         * @param currentRecipe Current authored recipe used for stale validation.
         * @return Success or typed failure leaving the prior snapshot intact.
         */
        [[nodiscard]] Result<void> Accept(OfflineVoronoiCandidate candidate, std::uint64_t expectedOwnerRevision,
                                          const OfflineVoronoiSource &currentSource, const OfflineVoronoiRecipe &currentRecipe);

        /** @brief Cancels current work and advances the revision, retaining the published snapshot. @return Success or typed failure. */
        [[nodiscard]] Result<void> Invalidate();
        /** @brief Closes acceptance and cancels current work, retaining published snapshots. */
        void Shutdown() noexcept;

    private:
        std::shared_ptr<const OfflineVoronoiCandidate> current_;
        CancellationSource cancellation_;
        std::uint64_t revision_{1};
        bool shutdown_{};
    };
}  // namespace Horo::Destruction
