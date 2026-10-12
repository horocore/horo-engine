#pragma once

/** @file TerrainRenderSnapshot.h
 * @brief Host-composed immutable visual candidates from exact resident Terrain cook generations. */

#include "Horo/Runtime/Render/Mesh.h"
#include "Horo/Terrain/TerrainFoliageRegistry.h"
#include "Horo/Terrain/TerrainMaterial.h"
#include "Horo/Terrain/TerrainPayloadManifest.h"
#include "Horo/WorldStreaming/OriginFrame.h"
#include "Horo/WorldStreaming/WorldStreamingIdentity.h"

#include <memory>
#include <optional>

namespace Horo::TerrainRender {
    /** @brief Complete source publication fence; generation is a non-wrapping host request sequence. */
    struct TerrainRenderSnapshotHeader final {
        std::uint32_t schema{1};
        Terrain::TerrainRuntimeHandle terrain{};
        Terrain::TerrainSnapshotRevision revisions{};
        std::uint64_t generation{};
        Terrain::TerrainConfigurationRevision configuration{};
        WorldStreaming::WorldPartitionId world{};
        std::optional<WorldStreaming::StreamingFence> cell{}; /**< Absent for standalone host scope. */
        WorldStreaming::OriginFrameBinding origin{};
        Math::Transform datasetToWorld{}; /**< Placement correlated with the exact origin publication. */
        Sha256Digest target{}, manifest{};
        Terrain::TerrainFoliageCapabilitySet capabilities{};
        bool operator==(const TerrainRenderSnapshotHeader &) const noexcept = default;
    };

    /** @brief Independent finite capture ceilings, checked before copying any selected geometry. */
    struct TerrainRenderSnapshotLimits final {
        std::uint32_t maximumTiles{256};
        std::uint64_t maximumVertices{1'048'576}, maximumIndices{6'291'456};
        std::uint64_t maximumOwnedBytes{128ULL * 1024 * 1024};
        std::uint64_t maximumWorkItems{8'388'608};
    };

    /** @brief Host-admitted exact visual artifact and semantic layers; no per-view selection or native identity. */
    struct TerrainRenderTileSelection final {
        Terrain::TerrainTileId tile{};
        Sha256Digest visual{};
        Terrain::TerrainMaterialLayerSetData layers{};
    };

    /** @brief View-independent legal tile/LOD with copied data and renderer-neutral cost facts. */
    struct TerrainTileRenderCandidate final {
        Terrain::TerrainTileId tile{};
        Sha256Digest visual{}, samples{};
        std::array<Sha256Digest, 4> seams{};
        std::array<double, 3> anchor{}; /**< Double dataset-space anchor; float vertices remain tile-local. */
        double maximumGeometricError{};
        bool requiresSameLodNeighbors{true}, holes{};
        Terrain::TerrainMaterialLayerSetData layers{};
        Render::MeshData mesh{};                   /**< Fully holed tiles retain vertices and explicitly empty indices. */
        std::vector<std::uint8_t> samplePayload{}; /**< Exact HTIL sample/layer/hole semantics for consumer decoding. */
    };

    /** @brief Invocation-only roots held by the Terrain authority's existing resident generation lease. */
    struct TerrainRenderSnapshotRequest final {
        TerrainRenderSnapshotHeader header{};
        Terrain::TerrainRuntimeLifecycle lifecycle{Terrain::TerrainRuntimeLifecycle::Closed};
        const Terrain::CookedTerrainSourceArtifacts &source;
        const Terrain::TerrainPayloadManifest &manifest;
        const Terrain::TerrainConfigurationSnapshot &configuration;
        std::span<const TerrainRenderTileSelection> tiles{};
        TerrainRenderSnapshotLimits limits{};
    };

    /** @brief Shared immutable CPU generation lease; no selected LOD, draw command, mutable runtime pointer or GPU ownership. */
    class TerrainRenderExtractionSnapshot final {
    public:
        /** @brief Reports lease presence. @return False for a moved-from snapshot. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns exact publication evidence. @return Empty evidence when moved from. */
        [[nodiscard]] const TerrainRenderSnapshotHeader &Header() const noexcept;
        /** @brief Returns complete canonical candidates without allocation. @return View valid while this lease is retained. */
        [[nodiscard]] std::span<const TerrainTileRenderCandidate> Tiles() const noexcept;
        /** @brief Returns admitted owned storage charge. @return Charge excluding allocator bookkeeping. */
        [[nodiscard]] std::uint64_t OwnedBytes() const noexcept;
        /** @brief Fences logical currentness independently of retained memory.
         * @param current Complete current owner evidence. @return Success or TerrainErrors::RevisionStale. */
        [[nodiscard]] Result<void> ValidateCurrent(const TerrainRenderSnapshotHeader &current) const;

    private:
        struct State;

        explicit TerrainRenderExtractionSnapshot(std::shared_ptr<const State> state) : state_(std::move(state)) {}

        friend Result<TerrainRenderExtractionSnapshot> CaptureTerrainRenderSnapshot(const TerrainRenderSnapshotRequest &,
                                                                                    const CancellationToken &);
        std::shared_ptr<const State> state_;
    };

    /** @brief Captures admitted resident visual artifacts into one detached immutable generation.
     * @param request Exact cook-issued roots, owner fence and complete selected set; no input is retained.
     * @param cancellation Cooperative observer checked across verification and geometry work.
     * @return Complete lease or typed invalid/stale/capacity/cancellation failure; never partial output.
     * @pre Host captures resident membership, unchanged root geometry, semantic layers and all revisions at its safe point.
     * @details Bounded load/preparation work with no I/O, shader compilation or native resource work. Multiple views reuse
     * the returned lease without recapture. A no-renderer host omits this integration; no capability fallback is inferred.
     * @throws std::bad_alloc Admitted storage allocation failed; detached work unwinds without publication. */
    [[nodiscard]] Result<TerrainRenderExtractionSnapshot> CaptureTerrainRenderSnapshot(const TerrainRenderSnapshotRequest &request,
                                                                                       const CancellationToken &cancellation = {});
}  // namespace Horo::TerrainRender
