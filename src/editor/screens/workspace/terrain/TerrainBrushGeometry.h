#pragma once
/** @file TerrainBrushGeometry.h @brief Private shared bounded source-grid stroke admission. */
#include "TerrainSculptProducer.h"

namespace Horo::Editor {
    /** @brief Exact detached source-grid footprint, admitted before channel/history allocation. */
    struct TerrainBrushFootprint final {
        TerrainPatchRect rect;
        std::size_t samples{};
        std::uint64_t chargedVisits{};
    };

    /** @brief Compute a clipped union rectangle and a conservative complete brush-work charge.
     * @param source Immutable canonical document source. @param radiusMeters Positive captured radius.
     * @param stamps Complete stroke. @param limits Explicit lowerable interaction ceilings.
     * @param sampleSetupVisits Channel reads to initialize each retained patch sample.
     * @param sampleStampVisits Upper bound for one sample per stamp, including normalization.
     * @param fixedVisits Fixed preparation charge, including selected-target normalization.
     * @return Exact footprint or typed invalid/capacity failure; no source samples or external state are mutated.
     */
    [[nodiscard]] Result<TerrainBrushFootprint> PrepareTerrainBrushFootprint(
        const Terrain::TerrainCanonicalSource &source, double radiusMeters, std::span<const TerrainSculptStamp> stamps,
        const TerrainSculptLimits &limits, std::uint64_t sampleSetupVisits, std::uint64_t sampleStampVisits, std::uint64_t fixedVisits);
}  // namespace Horo::Editor
