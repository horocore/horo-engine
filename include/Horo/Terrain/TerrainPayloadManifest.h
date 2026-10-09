#pragma once

/** @file TerrainPayloadManifest.h @brief Immutable Terrain/Foliage dataset membership for Assets generations. */

#include "Horo/Terrain/FoliageClusterCook.h"
#include "Horo/Terrain/TerrainSourceArtifacts.h"

namespace Horo::Terrain {
    /** @brief Portable manifest encoding version, independent of the payload schemas. */
    inline constexpr std::uint32_t CurrentTerrainPayloadManifestSchema = 1;

    namespace TerrainPayloadManifestErrors {
        extern const ErrorCodeDescriptor Invalid;       /**< Incompatible, incomplete or corrupt evidence. */
        extern const ErrorCodeDescriptor LimitExceeded; /**< Manifest construction exceeds a finite ceiling. */
        extern const ErrorCodeDescriptor Cancelled;     /**< Detached construction or admission cancelled. */
        extern const ErrorCodeDescriptor Stale;         /**< Exact generation expectation no longer matches. */
        extern const ErrorCodeDescriptor Closed;        /**< Host publication admission has closed. */
    }  // namespace TerrainPayloadManifestErrors

    /** @brief Consumer requirement, never evidence that a consumer is ready. No implicit fallback exists. */
    enum class TerrainPayloadRequirement : std::uint8_t {
        NotRequested,
        Optional,
        Required,
        Count
    };

    /** @brief Exact consumer requirements captured by the host before generation assembly. */
    struct TerrainPayloadRequirements final {
        TerrainPayloadRequirement visual{TerrainPayloadRequirement::NotRequested};
        TerrainPayloadRequirement collision{TerrainPayloadRequirement::NotRequested};
        TerrainPayloadRequirement navigation{TerrainPayloadRequirement::NotRequested};
        auto operator<=>(const TerrainPayloadRequirements &) const = default;
    };

    /** @brief Conservative inclusive world-metre envelope; flat axes are legal. */
    struct TerrainPayloadBounds final {
        std::array<double, 3> minimum{};
        std::array<double, 3> maximum{};
        auto operator<=>(const TerrainPayloadBounds &) const = default;
    };

    /** @brief One exact independently readable artifact; digest is its content-addressed identity. */
    struct TerrainPayloadArtifact final {
        std::uint32_t schema{};
        Sha256Digest digest{};
        std::uint64_t byteSize{};
        std::uint64_t decodedBytes{}; /**< Neutral owned decode estimate, excluding consumer-native allocations. */
        std::uint64_t workItems{};    /**< Neutral decoded elements, not a scheduler allocation. */
        auto operator<=>(const TerrainPayloadArtifact &) const = default;
    };

    /** @brief Complete one-tile/one-LOD descriptor. Consumer role indices follow TerrainSourceArtifactRole. */
    struct TerrainTilePayloadEntry final {
        TerrainTileId tile{};
        std::uint32_t samplesX{}, samplesZ{};
        TerrainPayloadBounds bounds{};
        std::array<Sha256Digest, 4> seams{}; /**< West/east/north/south; same-LOD neighbours must match. */
        TerrainPayloadArtifact samples{};
        std::array<std::optional<TerrainPayloadArtifact>, 3> consumers{};
        double maximumGeometricError{};
        bool requiresSameLodNeighbors{true};
    };

    /** @brief Complete immutable cluster descriptor with exact placement/geometry provenance and integer bounds. */
    struct FoliageClusterPayloadEntry final {
        TerrainTileId tile{};
        FoliageTypeId type{};
        FoliageClusterId cluster{};
        TerrainSourceRevision source{};
        FoliageDefinitionRevision definition{};
        TerrainContentRevision placement{};
        Sha256Digest placementFingerprint{}, placementDigest{}, geometryDigest{}, profileFingerprint{};
        FoliageClusterBounds bounds{};
        std::uint32_t geometryRadiusMillimeters{};
        std::uint64_t instanceCount{};
        TerrainPayloadArtifact instances{};
    };

    /** @brief Exact dataset generation provenance. No paths, runtime handles or provider-native identities. */
    struct TerrainPayloadProvenance final {
        TerrainDatasetId dataset{};
        TerrainContentRevision content{};
        Assets::AssetId sourceAsset{};
        TerrainSourceRevision source{};
        TerrainCapabilityRevision capability{};
        TerrainFeatureTier tier{TerrainFeatureTier::Baseline};
        Sha256Digest sourceDigest{}, targetDigest{}, toolchainDigest{};
        Sha256Digest tileFingerprint{}, tileManifestDigest{}, geometryFingerprint{}, geometryManifestDigest{};
        Sha256Digest foliageFingerprint{}, foliageManifestDigest{}, terrainDependencies{}, foliageDependencies{};
    };

    /** @brief Finite tooling-only construction ceilings; these grant no runtime/WST budget. */
    struct TerrainPayloadManifestLimits final {
        std::uint32_t maximumTiles{2'048}, maximumClusters{8'192}, maximumDependencies{4'096};
        std::uint64_t maximumInputBytes{1024ULL * 1024 * 1024};
        std::uint64_t maximumOwnedBytes{16ULL * 1024 * 1024};
        std::uint64_t maximumWorkItems{8'388'608};
    };

    class TerrainPayloadManifest;

    /** @brief Complete cook-issued input membership borrowed only until generation returns. */
    struct TerrainPayloadManifestRequest final {
        const CookedTerrainSourceArtifacts *terrain{};
        const CookedFoliageClusterSet *foliage{}; /**< Null explicitly excludes foliage; an empty generation remains distinguishable. */
        TerrainContentRevision content{};
        std::span<const TerrainTileCookDependency> terrainDependencies{}; /**< Must reproduce the exact tile cook key. */
        std::span<const TerrainTileCookDependency> foliageDependencies{}; /**< Host-verified exact closure, including geometry digests. */
        Sha256Digest verifiedFoliageDependencyClosure{}; /**< Expected canonical closure digest from the Assets dependency snapshot. */
        TerrainPayloadRequirements terrainRequirements{}, foliageRequirements{};
        TerrainPayloadManifestLimits limits{};
        const TerrainPayloadManifest *previous{}; /**< Optional exact same-dataset predecessor; retained roots are never mutated. */
    };

    /** @brief Cook-issued immutable descriptor root and canonical HTPM bytes; Assets owns durable publication and byte leases. */
    class TerrainPayloadManifest final {
    public:
        TerrainPayloadManifest(const TerrainPayloadManifest &) = default;
        TerrainPayloadManifest(TerrainPayloadManifest &&) noexcept = default;
        TerrainPayloadManifest &operator=(const TerrainPayloadManifest &) = delete;
        TerrainPayloadManifest &operator=(TerrainPayloadManifest &&) = delete;

        /** @brief Returns exact generation provenance. @return Immutable owned facts. */
        [[nodiscard]] const TerrainPayloadProvenance &Provenance() const noexcept {
            return provenance_;
        }

        /** @brief Returns canonical LOD/Z/X tile descriptors. @return Borrow valid until this root is moved or destroyed. */
        [[nodiscard]] std::span<const TerrainTilePayloadEntry> Tiles() const noexcept {
            return tiles_;
        }

        /** @brief Returns canonical tile/type/cluster descriptors. @return Borrow tied to this root. */
        [[nodiscard]] std::span<const FoliageClusterPayloadEntry> Clusters() const noexcept {
            return clusters_;
        }

        /** @brief Returns conservative complete dataset bounds. @return Immutable world-metre envelope. */
        [[nodiscard]] const TerrainPayloadBounds &Bounds() const noexcept {
            return bounds_;
        }

        /** @brief Returns exact source coordinate interpretation. @return Immutable owned coordinates. */
        [[nodiscard]] const TerrainSourceCoordinates &Coordinates() const noexcept {
            return coordinates_;
        }

        /** @brief Returns terrain consumer requirements. @return Requirements only, never readiness receipts. */
        [[nodiscard]] TerrainPayloadRequirements TerrainRequirements() const noexcept {
            return terrainRequirements_;
        }

        /** @brief Returns foliage consumer requirements. @return Exact declared requirements. */
        [[nodiscard]] TerrainPayloadRequirements FoliageRequirements() const noexcept {
            return foliageRequirements_;
        }

        /** @brief Returns verified terrain dependencies in AssetId order. @return Borrow tied to this root. */
        [[nodiscard]] std::span<const TerrainTileCookDependency> TerrainDependencies() const noexcept {
            return terrainDependencies_;
        }

        /** @brief Returns host-verified foliage dependency closure. @return Borrow tied to this root. */
        [[nodiscard]] std::span<const TerrainTileCookDependency> FoliageDependencies() const noexcept {
            return foliageDependencies_;
        }

        /** @brief Returns canonical versioned bytes. @return Owned immutable descriptor encoding without payload duplication. */
        [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept {
            return bytes_;
        }

        /** @brief Returns exact manifest artifact identity. @return SHA-256 over Bytes. */
        [[nodiscard]] Sha256Digest Digest() const noexcept {
            return digest_;
        }

        /** @brief Returns retained descriptor allocation capacity. @return Includes object, vectors, CRS and encoded bytes. */
        [[nodiscard]] std::uint64_t OwnedBytes() const noexcept;

    private:
        friend struct TerrainPayloadManifestBuilder;
        TerrainPayloadManifest() = default;
        TerrainPayloadProvenance provenance_{};
        TerrainPayloadBounds bounds_{};
        TerrainSourceCoordinates coordinates_{};
        TerrainPayloadRequirements terrainRequirements_{}, foliageRequirements_{};
        std::vector<TerrainTilePayloadEntry> tiles_{};
        std::vector<FoliageClusterPayloadEntry> clusters_{};
        std::vector<TerrainTileCookDependency> terrainDependencies_{}, foliageDependencies_{};
        std::vector<std::uint8_t> bytes_{};
        Sha256Digest digest_{};
    };

    /**
     * @brief Generates complete canonical dataset membership from verified Terrain/Foliage cook outputs.
     * @param request Exact provenance, dependency closure, requirements and finite construction limits.
     * @param cancellation Host-owned cooperative observer checked throughout bounded work.
     * @return Owned complete manifest or typed failure, preserving every prior root.
     * @throws std::bad_alloc Admitted allocation failure unwinds the detached candidate without publication.
     * This background/tooling call retains no input, callback or task. The host cancels and joins its operation
     * before input shutdown. Assets publishes the returned root through its existing generation transaction.
     */
    [[nodiscard]] Result<TerrainPayloadManifest> GenerateTerrainPayloadManifest(const TerrainPayloadManifestRequest &request,
                                                                                const CancellationToken &cancellation = {});

    /**
     * @brief Computes the canonical closure digest for host-verified dependency records.
     * @param dependencies Exact provider-verified artifacts; order is normalized and duplicate identities fail.
     * @return Digest or typed invalid/limit error. This computes identity, not provider authenticity.
     * Assets supplies the resulting trusted snapshot digest separately from the assembly inputs.
     */
    [[nodiscard]] Result<Sha256Digest> TerrainPayloadDependencyClosureDigest(std::span<const TerrainTileCookDependency> dependencies);

    /**
     * @brief Revalidates bytes or independently loaded manifest data against an exact cook-issued root.
     * @param expected Selected immutable generation; moved-from roots fail closed.
     * @param bytes Loaded manifest bytes; size/schema/digest must match the trusted generation.
     * @param cancellation Cooperative observer checked between hash chunks.
     * @return Success or typed corrupt/cancelled result. No parsing, repair, I/O or publication occurs.
     */
    [[nodiscard]] Result<void> VerifyTerrainPayloadManifest(const TerrainPayloadManifest &expected, std::span<const std::uint8_t> bytes,
                                                            const CancellationToken &cancellation = {});

    /**
     * @brief Pure Assets publication-gate check for insertion, exact replacement, cancellation and shutdown.
     * @param candidate Complete detached root to admit.
     * @param current Exact current root, null for insertion.
     * @param expectedCurrent Current manifest digest, absent for insertion.
     * @param lifecycle Captured host owner lifecycle; only Active admits publication.
     * @param cancellation Host safe-point cancellation observer.
     * @return Success or typed invalid/stale/closed/cancelled result; caller alone owns atomic publication.
     */
    [[nodiscard]] Result<void> ValidateTerrainPayloadManifestPublication(const TerrainPayloadManifest &candidate,
                                                                         const TerrainPayloadManifest *current,
                                                                         std::optional<Sha256Digest> expectedCurrent,
                                                                         TerrainRuntimeLifecycle lifecycle,
                                                                         const CancellationToken &cancellation = {});
}  // namespace Horo::Terrain
