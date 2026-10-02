#pragma once

/**
 * @file ChunkMeshCook.h
 * @brief Portable chunk surface cook and revision-fenced immutable publication.
 */

#include "Horo/Destruction/OfflineVoronoi.h"
#include "Horo/Destruction/PreFracturedImport.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Horo::Destruction {
    inline constexpr std::uint32_t ChunkMeshCookSchemaVersion = 2;

    struct CollisionPieceIdentityTag;
    /** @brief Geometry-derived chunk-local identity, independent of piece array order or Physics target. */
    using CollisionPieceId = DestructionStableIdentity<CollisionPieceIdentityTag>;

    /** @brief Solver-neutral closed collision region retained for independent Physics cooking. */
    struct ChunkCollisionPiece final {
        CollisionPieceId id{};
        std::vector<std::array<float, 3>> positions;
        std::vector<std::array<std::uint32_t, 3>> triangles;
        double volume{};
    };

    namespace ChunkMeshCookErrors {
        extern const ErrorCodeDescriptor InvalidInput;
        extern const ErrorCodeDescriptor InvalidInterior;
        extern const ErrorCodeDescriptor MissingMaterial;
        extern const ErrorCodeDescriptor LimitExceeded;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor Shutdown;
    }  // namespace ChunkMeshCookErrors

    /** @brief Stable logical material binding; one slot has exactly one required asset. */
    struct ChunkMaterialBinding final {
        std::uint32_t slot{};
        Assets::AssetId asset{};
        Sha256Digest revisionDigest{}; /**< Exact material dependency revision, never a display name. */
    };

    /** @brief Explicit imported source-name mapping; names are cook input only, never runtime identity. */
    struct ImportedChunkMaterialBinding final {
        std::string sourceName;
        ChunkMaterialBinding material;
        bool interior{};
    };

    /** @brief Explicit planar UV scale in reciprocal Horo units. */
    struct ChunkUvPolicy final {
        double exteriorScale{1.0};
        double interiorScale{1.0};
    };

    /** @brief Captured imported-source identity, content, tier and finite cook policy. */
    struct ImportedChunkMeshCookRequest final {
        Assets::AssetId sourceAsset{};
        std::uint64_t sourceRevision{};
        Sha256Digest sourceDigest{};
        FractureArtifactContentIdentity content{};
        DestructionFeatureTier tier{};
        ChunkUvPolicy uv{};
        DestructionLimits limits{};
    };

    /** @brief One canonical vertex, with face-local basis to preserve sharp cut boundaries. */
    struct ChunkMeshVertex final {
        std::array<float, 3> position{};
        std::array<float, 3> normal{};
        std::array<float, 4> tangent{}; /**< XYZ axis and signed bitangent handedness. */
        std::array<float, 2> uv{};
    };

    /** @brief Canonical face semantics and logical material binding. */
    struct ChunkMeshFace final {
        std::array<std::uint32_t, 3> indices{};
        std::uint32_t materialSlot{};
        bool interior{};
    };

    /** @brief Finite local bounds and density-independent center-of-mass integration inputs. */
    struct ChunkMeshMassInputs final {
        std::array<float, 3> minimum{};
        std::array<float, 3> maximum{};
        double volume{};
        std::array<double, 3> firstMoment{}; /**< Volume times center of mass. */
    };

    /** @brief One immutable semantic chunk mesh. */
    struct ChunkMesh final {
        DestructionChunkId id{};
        std::vector<ChunkMeshVertex> vertices;
        std::vector<ChunkMeshFace> faces;
        ChunkMeshMassInputs mass{};
        std::vector<ChunkCollisionPiece> collisionPieces;
    };

    /** @brief Complete detached artifact payload, owned only through a const shared snapshot. */
    struct ChunkMeshArtifact final {
    private:
        struct CookKey final {
            CookKey(const CookKey &) = default;

        private:
            CookKey() = default;
            friend struct ChunkMeshArtifact;
        };

        static CookKey ConstructionKey() {
            return {};
        }

    public:
        /** @brief Constructs a detached artifact only for authorized cooks. @param key Cook construction key. */
        explicit ChunkMeshArtifact([[maybe_unused]] const CookKey &key) {}

        ChunkMeshArtifact(const ChunkMeshArtifact &) = delete;
        ChunkMeshArtifact &operator=(const ChunkMeshArtifact &) = delete;
        ChunkMeshArtifact(ChunkMeshArtifact &&) = delete;
        ChunkMeshArtifact &operator=(ChunkMeshArtifact &&) = delete;

        FractureArtifactContentIdentity content{};
        Assets::AssetId sourceAsset{};
        std::uint64_t sourceRevision{};
        Sha256Digest sourceDigest{};
        std::uint64_t recipeId{};
        std::uint64_t recipeRevision{};
        Sha256Digest inputFingerprint{};
        Sha256Digest materialFingerprint{};
        Sha256Digest integrityDigest{};
        DestructionFeatureTier tier{};            /**< Exact admitted DFR product tier. */
        DestructionFeatureSet producedFeatures{}; /**< Canonical features present in this artifact. */
        std::uint32_t schemaVersion{ChunkMeshCookSchemaVersion};
        std::vector<ChunkMaterialBinding> materials;
        std::vector<ChunkMesh> chunks;
        std::uint64_t estimatedBytes{};
        std::uint64_t workItems{};

    private:
        friend Result<std::shared_ptr<const ChunkMeshArtifact>> CookChunkMeshes(const OfflineVoronoiCandidate &,
                                                                                const FractureArtifactContentIdentity &,
                                                                                std::span<const ChunkMaterialBinding>, ChunkUvPolicy,
                                                                                const DestructionLimits &, const CancellationToken &);
        friend Result<std::shared_ptr<const ChunkMeshArtifact>> CookPreFracturedChunkMeshes(const PreFracturedCandidate &,
                                                                                            const ImportedChunkMeshCookRequest &,
                                                                                            std::span<const ImportedChunkMaterialBinding>,
                                                                                            const CancellationToken &);
    };

    /** @brief Verifies a sealed mesh artifact before independent derived cooking.
     * @param artifact Immutable authorized mesh cook result.
     * @return Success or typed schema/integrity failure without mutation.
     */
    [[nodiscard]] Result<void> ValidateChunkMeshArtifact(const ChunkMeshArtifact &artifact);

    /**
     * @brief Computes the exact content digest for this mesh cook's semantic inputs.
     * @param source Validated fracture candidate.
     * @param materials Complete ordered material bindings.
     * @param uv Explicit UV policy.
     * @return Canonical digest to bind into FractureArtifactContentIdentity.
     */
    [[nodiscard]] Sha256Digest ComputeChunkMeshSemanticDigest(const OfflineVoronoiCandidate &source,
                                                              std::span<const ChunkMaterialBinding> materials, ChunkUvPolicy uv);

    /** @brief Hashes validated imported chunk geometry, hierarchy and source material names in stable-ID order.
     * @param source Detached validated pre-fractured candidate.
     * @return Portable digest independent of source paths and display labels.
     */
    [[nodiscard]] Sha256Digest ComputePreFracturedMeshSourceDigest(const PreFracturedCandidate &source);

    /** @brief Binds imported source content, exact material mappings and UV policy to the mesh schema.
     * @param source Validated pre-fractured candidate.
     * @param sourceAsset Stable source asset identity.
     * @param sourceRevision Exact nonzero source revision.
     * @param tier Exact selected DFR product tier.
     * @param materials Complete ordered source-name mappings.
     * @param uv Explicit UV policy.
     * @return Canonical semantic content digest.
     */
    [[nodiscard]] Sha256Digest ComputePreFracturedMeshSemanticDigest(const PreFracturedCandidate &source, Assets::AssetId sourceAsset,
                                                                     std::uint64_t sourceRevision, DestructionFeatureTier tier,
                                                                     std::span<const ImportedChunkMaterialBinding> materials,
                                                                     ChunkUvPolicy uv);

    /**
     * @brief Cooks complete face geometry from a detached offline fracture result.
     * @param source Complete Voronoi candidate, with stable IDs and exact provenance.
     * @param content Exact fracture-asset revision for the eventual publication.
     * @param materials Sorted required material bindings for every referenced slot.
     * @param uv Explicit finite planar UV policy.
     * @param limits Product limits bounding decoded bytes and work.
     * @param cancellation Cooperative cancellation observed between bounded units.
     * @return Immutable detached artifact or typed failure without partial publication.
     */
    [[nodiscard]] Result<std::shared_ptr<const ChunkMeshArtifact>> CookChunkMeshes(const OfflineVoronoiCandidate &source,
                                                                                   const FractureArtifactContentIdentity &content,
                                                                                   std::span<const ChunkMaterialBinding> materials,
                                                                                   ChunkUvPolicy uv, const DestructionLimits &limits,
                                                                                   const CancellationToken &cancellation);

    /**
     * @brief Cooks validated imported chunks with explicit interior/material attribution.
     * @param source Complete normalized pre-fractured candidate.
     * @param request Captured source identity, exact fracture content, tier, UV policy and finite limits.
     * @param materials Sorted unique source-name mappings, including every used material.
     * @param cancellation Cooperative cancellation token.
     * @return Immutable artifact or typed failure with no partial publication.
     */
    [[nodiscard]] Result<std::shared_ptr<const ChunkMeshArtifact>> CookPreFracturedChunkMeshes(
        const PreFracturedCandidate &source, const ImportedChunkMeshCookRequest &request,
        std::span<const ImportedChunkMaterialBinding> materials, const CancellationToken &cancellation);

    /** @brief Single-thread owner for atomic candidate replacement; callers serialize on the owning thread. */
    class ChunkMeshCookOwner final {
    public:
        ChunkMeshCookOwner() = default;
        ChunkMeshCookOwner(const ChunkMeshCookOwner &) = delete;
        ChunkMeshCookOwner &operator=(const ChunkMeshCookOwner &) = delete;

        /** @brief Current owner generation. @return Nonzero non-wrapping revision. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;

        /** @brief Token for detached work in this generation. @return Current cancellation token. */
        [[nodiscard]] CancellationToken Token() const noexcept;

        /** @brief Last complete published value. @return Immutable snapshot or null. */
        [[nodiscard]] std::shared_ptr<const ChunkMeshArtifact> Snapshot() const noexcept {
            return current_;
        }

        /**
         * @brief Publishes only a complete candidate from the exact current generation and content.
         * @param candidate Immutable detached artifact.
         * @param expectedRevision Revision captured before work.
         * @param currentContent Current fracture content identity.
         * @return Success or typed failure retaining the previous snapshot.
         */
        [[nodiscard]] Result<void> Accept(std::shared_ptr<const ChunkMeshArtifact> candidate, std::uint64_t expectedRevision,
                                          const FractureArtifactContentIdentity &currentContent);
        /** @brief Cancels pending work and advances the generation. @return Success or exhaustion/shutdown failure. */
        [[nodiscard]] Result<void> Invalidate();
        /** @brief Closes admission and cancels pending work while retaining the last snapshot. */
        void Shutdown() noexcept;

    private:
        std::shared_ptr<const ChunkMeshArtifact> current_;
        CancellationSource cancellation_;
        std::uint64_t revision_{1};
        bool shutdown_{};
    };
}  // namespace Horo::Destruction
