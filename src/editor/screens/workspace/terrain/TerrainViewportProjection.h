#pragma once
/** @file TerrainViewportProjection.h @brief Private source geometry projection shared by terrain and foliage authoring overlays. */
#include "TerrainViewportTool.h"

namespace Horo::Editor {
    /** @brief Sample the actual canonical triangle, with explicit retained-heightfield restoration mode. */
    [[nodiscard]] std::optional<Math::Vec3> SampleTerrainPreview(const Terrain::TerrainCanonicalSource &source, double x, double z,
        TerrainSurfacePickMode mode = TerrainSurfacePickMode::VisibleSurface);
    /** @brief Project a genuine source-local point through the exact preview transform/camera. */
    [[nodiscard]] std::optional<ImVec2> ProjectTerrainPreview(const TerrainViewportTool::Frame &frame, Math::Vec3 point);
}
