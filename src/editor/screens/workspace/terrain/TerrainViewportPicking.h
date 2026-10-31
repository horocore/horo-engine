#pragma once
/** @file TerrainViewportPicking.h @brief Private bounded ray/heightfield picking shared by real workspace tools. */
#include "Horo/Math/SceneMath.h"
#include "TerrainSculptProducer.h"

namespace Horo::Editor {
    /** @brief Real canonical surface hit; sample coordinates retain sub-sample brush placement. */
    struct TerrainSurfaceHit final {
        TerrainSculptStamp stamp;
        Math::Vec3 viewportPoint;
        float heightMeters{};
        double distance{};
    };

    /** @brief Intersect actual source triangles using bounded grid traversal, with no plane fallback.
     * @param source Canonical height-only document source. @param datasetToViewport Captured transform of the first sample origin;
     * the host already resolves double source coordinates through the current viewport origin frame.
     * @param ray Normalized finite viewport ray segment. @param maximumVisitedCells Finite DDA work ceiling.
     * @param cancellation Cooperative capture. @return Nearest real hit, no hit, or typed invalid/work/cancellation failure.
     */
    [[nodiscard]] Result<std::optional<TerrainSurfaceHit>> PickTerrainSurface(const Terrain::TerrainCanonicalSource &source,
                                                                              const Math::Transform &datasetToViewport,
                                                                              const Math::Ray &ray, std::size_t maximumVisitedCells = 4096,
                                                                              const CancellationToken &cancellation = {});
}  // namespace Horo::Editor
