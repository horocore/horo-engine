#pragma once
/** @file TerrainPaintProducer.h @brief Private normalized semantic-layer painting and binary hole patch production. */
#include "Horo/Terrain/TerrainAuthoringSourceCodec.h"
#include "TerrainSculptProducer.h"

namespace Horo::Editor {
    enum class TerrainPaintMode : std::uint8_t {
        Weights,
        CarveHole,
        RestoreHole
    };

    /** @brief One active typed layer target; inactive layers remain stored and reachable through the full palette. */
    struct TerrainPaintTarget final {
        Terrain::TerrainLayerId layer;
        float weight{1.0F};
    };

    /** @brief Transient stroke parameters, never canonical storage or history. */
    struct TerrainPaintBrush final {
        TerrainPaintMode mode{TerrainPaintMode::Weights};
        double radiusMeters{2};
        float strength{0.25F};
        std::array<TerrainPaintTarget, 4> targets{};
        std::size_t targetCount{};
    };

    /** @brief Source fence plus the exact persisted semantic layer meaning captured by this gesture. */
    struct TerrainPaintFence final {
        TerrainEditFence document;
        Terrain::TerrainLayerSetId layerSet;
        Terrain::TerrainContentRevision layerRevision;
        Sha256Digest layerDigest;
    };

    /** @brief Produce one exact normalized weight or binary-hole patch through immutable canonical borrows.
     * @param source Actual canonical document channels. @param binding Full stored layer order, already matched to the captured asset.
     * @param fence Exact source and semantic-layer fence. @param operation Host-issued correlation.
     * @param brush Captured closed tool parameters. @param stamps Complete bounded sample-space gesture.
     * @param limits Lowerable source-region/work ceilings. @param cancellation Checked before allocation and per row/stamp.
     * @return One exact patch operation or typed error, with source and histories unchanged.
     * @details At most four active paint targets blend into all stored channels, followed by canonical integer normalization.
     * Carve/restore use exact binary values inside the radius; a partial hole/opacity channel is never invented.
     */
    [[nodiscard]] Result<TerrainEditOperation> ProduceTerrainPaint(
        const Terrain::TerrainCanonicalSource &source, const std::optional<Terrain::TerrainAuthoringLayerBinding> &binding,
        const TerrainPaintFence &fence, TerrainEditOperationId operation, const TerrainPaintBrush &brush,
        std::span<const TerrainSculptStamp> stamps, const TerrainSculptLimits &limits = {}, const CancellationToken &cancellation = {});
}  // namespace Horo::Editor
