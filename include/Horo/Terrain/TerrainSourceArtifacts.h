#pragma once

/** @file TerrainSourceArtifacts.h @brief Immutable offline Terrain geometry for consumer-owned native conversion. */

#include "Horo/Terrain/TerrainTileCook.h"

namespace Horo::Terrain {
    /** @brief Independent neutral geometry schema; does not change HTIL/HTMF v1. */
    inline constexpr std::uint32_t CurrentTerrainSourceArtifactSchema = 1;

    /** @brief Exact artifact role, never a native resource or capability fallback. */
    enum class TerrainSourceArtifactRole : std::uint8_t {
        Visual,
        Collision,
        Navigation
    };

    /** @brief Canonical right-handed Y-up meter position; projected-world precision is retained in doubles. */
    struct TerrainSourceVertex final {
        double x{}, y{}, z{};
        auto operator<=>(const TerrainSourceVertex &) const = default;
    };

    /** @brief Upward-wound indexed triangle and its exact source-grid quad coverage. */
    struct TerrainSourceTriangle final {
        std::array<std::uint32_t, 3> indices{};
        std::uint32_t beginX{}, endX{}, beginZ{}, endZ{};
        auto operator<=>(const TerrainSourceTriangle &) const = default;
    };

    /** @brief Captured contribution policy with explicit consumer LODs and aggregate geometry ceilings. */
    struct TerrainSourceArtifactProfile final {
        TerrainTileCookProfile tiles{}; /**< Existing exact tiling, target, toolchain and dependency policy. */
        std::uint8_t collisionLod{};    /**< Required available LOD, not clamped to a different representation. */
        std::uint8_t navigationLod{};   /**< Required available LOD; no native NavMesh is generated. */
        std::uint64_t maximumVertices{1'048'576};
        std::uint64_t maximumTriangles{2'097'152};
        std::uint64_t maximumOwnedBytes{128ULL * 1024 *
                                        1024}; /**< Peak candidate capacities, including tile/CRS, geometry, manifests and grid scratch. */
        std::uint64_t maximumWorkItems{
            1'048'576}; /**< Geometry visits, including conservative holes/error; tile cook has its own ceiling. */
    };

    /** @brief Owned neutral geometry and independently verifiable canonical artifact bytes. */
    struct TerrainSourceArtifact final {
        TerrainTileId tile{};
        TerrainSourceArtifactRole role{TerrainSourceArtifactRole::Visual};
        std::array<Sha256Digest, 4> seams{}; /**< Exact matching tile-source edge signatures. */
        double maximumGeometricError{};      /**< Conservative vertical error bound over canonical piecewise-linear source geometry. */
        bool requiresSameLodNeighbors{true}; /**< No skirt/morph is invented by a consumer; mixed LODs are not admitted. */
        std::vector<TerrainSourceVertex> vertices{};
        std::vector<TerrainSourceTriangle> triangles{};
        std::vector<std::uint8_t> payload{}; /**< HTSG v1; provenance plus complete geometry, not a metadata-only reference. */
        Sha256Digest digest{};
    };

    /** @brief Move-only complete offline candidate; Assets retains storage/publication authority. */
    class CookedTerrainSourceArtifacts final {
    public:
        CookedTerrainSourceArtifacts(const CookedTerrainSourceArtifacts &) = delete;
        CookedTerrainSourceArtifacts &operator=(const CookedTerrainSourceArtifacts &) = delete;
        CookedTerrainSourceArtifacts(CookedTerrainSourceArtifacts &&) noexcept = default;
        CookedTerrainSourceArtifacts &operator=(CookedTerrainSourceArtifacts &&) = delete;

        /** @brief Returns verified original height/weight/hole tiles. @return Immutable view tied to this candidate. */
        [[nodiscard]] const CookedTerrainTileSet &Tiles() const noexcept {
            return tiles_;
        }

        /** @brief Returns canonical tile/role ordered geometry. @return Immutable owned view. */
        [[nodiscard]] std::span<const TerrainSourceArtifact> Artifacts() const noexcept {
            return artifacts_;
        }

        /** @brief Returns exact source capability captured by this cook. @return Non-zero capability revision. */
        [[nodiscard]] TerrainCapabilityRevision Capability() const noexcept {
            return capability_;
        }

        /** @brief Returns complete policy fingerprint. @return Source/tile/consumer-schema and policy digest. */
        [[nodiscard]] Sha256Digest Fingerprint() const noexcept {
            return fingerprint_;
        }

        /** @brief Returns complete canonical manifest. @return Immutable HTSM v1 bytes. */
        [[nodiscard]] std::span<const std::uint8_t> Manifest() const noexcept {
            return manifest_;
        }

        /** @brief Returns manifest integrity. @return SHA-256 of Manifest. */
        [[nodiscard]] Sha256Digest ManifestDigest() const noexcept {
            return manifestDigest_;
        }

        /**
         * @brief Revalidates a candidate before host publication against an authoritative source capture.
         * @param source Immutable source borrowed for this call; its identity, revision, capability and all bytes must match.
         * @return Success or typed stale/invalid result; no native resource, registry or cache changes.
         */
        [[nodiscard]] Result<void> ValidateCurrent(const TerrainCanonicalSource &source) const;

    private:
        friend Result<CookedTerrainSourceArtifacts> CookTerrainSourceArtifacts(const TerrainCanonicalSource &,
                                                                               const TerrainSourceArtifactProfile &,
                                                                               std::span<const TerrainTileCookDependency>,
                                                                               const CancellationToken &);
        CookedTerrainSourceArtifacts(CookedTerrainTileSet tiles, TerrainCapabilityRevision capability, const Sha256Digest &fingerprint,
                                     std::vector<TerrainSourceArtifact> artifacts, std::vector<std::uint8_t> manifest);

        CookedTerrainTileSet tiles_;
        TerrainCapabilityRevision capability_;
        Sha256Digest fingerprint_;
        std::vector<TerrainSourceArtifact> artifacts_;
        std::vector<std::uint8_t> manifest_;
        Sha256Digest manifestDigest_;
    };

    /**
     * @brief Generates actual visual LODs and hole-aware canonical collision/navigation triangle sources offline.
     * @param source Borrowed canonical source, immutable until the call returns.
     * @param profile Exact roles, available consumer LODs and finite aggregate ceilings.
     * @param dependencies Exact generic Assets dependency artifacts, canonically ordered by the contribution.
     * @param cancellation Host-owned cooperative token; cancellation returns no candidate.
     * @return Complete uniquely owned candidate or typed failure. No I/O, scheduler, native cooking or runtime activation.
     * @throws std::bad_alloc If admitted allocation fails; stack-owned candidates unwind without publication.
     *
     * This load/tooling-only call has no retained source borrows, tasks or callbacks. Shutdown cancels/joins the
     * host operation before releasing its inputs; completed results remain valid after source/host retirement.
     * Hole policy v1 omits a coarse quad if any original sample in its closed coverage is a hole. All vertices
     * retain canonical heights (heightScale/offset are not applied a second time). Same-LOD seams only are legal.
     */
    [[nodiscard]] Result<CookedTerrainSourceArtifacts> CookTerrainSourceArtifacts(const TerrainCanonicalSource &source,
                                                                                  const TerrainSourceArtifactProfile &profile,
                                                                                  std::span<const TerrainTileCookDependency> dependencies,
                                                                                  const CancellationToken &cancellation);

    /**
     * @brief Verifies independently loaded HTSG bytes before consumer-owned conversion.
     * @param payload Immutable artifact bytes from an Assets lease.
     * @param expectedDigest Exact digest selected by the verified generation manifest, never recomputed as a trust bypass.
     * @param cancellation Cooperative observer checked between 4096-byte hash chunks and individual geometry records.
     * @return Success or typed corruption error for malformed version, provenance, coordinates, bounds or topology.
     *
     * The host also matches artifact tile/role membership to its selected manifest. This function performs no
     * allocation, native conversion, registration or repair and cannot prove runtime consumer readiness.
     */
    [[nodiscard]] Result<void> VerifyTerrainSourceArtifactPayload(std::span<const std::uint8_t> payload, const Sha256Digest &expectedDigest,
                                                                  const CancellationToken &cancellation = {});
}  // namespace Horo::Terrain
