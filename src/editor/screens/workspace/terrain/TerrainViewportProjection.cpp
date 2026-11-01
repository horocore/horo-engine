#include "TerrainViewportProjection.h"

#include "editor/renderer/EditorViewportScene.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    namespace {
        bool ValidSource(const Terrain::TerrainCanonicalSource &source) {
            return source.width >= 2 && source.height >= 2 && source.heightsMeters.size() == std::uint64_t(source.width) * source.height &&
                   (source.holes.empty() || source.holes.size() == source.heightsMeters.size());
        }

        bool ValidPosition(const Terrain::TerrainCanonicalSource &source, double x, double z) {
            if (!ValidSource(source))
                return false;
            return std::isfinite(x) && std::isfinite(z) && x >= 0 && z >= 0 && x <= source.width - 1 && z <= source.height - 1;
        }

        bool VisibleCell(const Terrain::TerrainCanonicalSource &source, std::size_t index) {
            if (source.holes.empty())
                return true;
            return source.holes[index] == 0 && source.holes[index + 1] == 0 && source.holes[index + source.width] == 0 &&
                   source.holes[index + source.width + 1] == 0;
        }
    }  // namespace

    /** @copydoc SampleTerrainPreview */
    std::optional<Math::Vec3> SampleTerrainPreview(const Terrain::TerrainCanonicalSource &source, double x, double z,
                                                   TerrainSurfacePickMode mode) {
        if (!ValidPosition(source, x, z))
            return {};
        const auto ix = static_cast<std::uint32_t>(std::min(std::floor(x), double(source.width - 2)));
        const auto iz = static_cast<std::uint32_t>(std::min(std::floor(z), double(source.height - 2)));
        const auto index = std::size_t(iz) * source.width + ix;
        if (mode == TerrainSurfacePickMode::VisibleSurface && !VisibleCell(source, index))
            return {};
        const double u = x - ix, v = z - iz;
        const double a = source.heightsMeters[index], b = source.heightsMeters[index + 1], c = source.heightsMeters[index + source.width],
                     d = source.heightsMeters[index + source.width + 1];
        const double height = u + v <= 1 ? a + (b - a) * u + (c - a) * v : d + (c - d) * (1 - u) + (b - d) * (1 - v);
        Math::Vec3 point{static_cast<float>(x * source.coordinates.spacingX), static_cast<float>(height),
                         static_cast<float>(z * source.coordinates.spacingZ)};
        return Math::IsFinite(point) ? std::optional{point} : std::nullopt;
    }

    /** @copydoc ProjectTerrainPreview */
    std::optional<ImVec2> ProjectTerrainPreview(const TerrainViewportTool::Frame &frame, Math::Vec3 point) {
        auto matrix = frame.datasetToViewport.TryToMatrix();
        if (matrix.HasError())
            return {};
        auto world = Math::TryTransformPoint(matrix.Value(), point);
        if (world.HasError())
            return {};
        auto projected = ProjectEditorViewportPoint(frame.camera, world.Value(), frame.size.x / frame.size.y, frame.depth);
        if (projected.HasError() || !projected.Value())
            return {};
        return ImVec2{frame.origin.x + projected.Value()->viewportPosition.x * frame.size.x,
                      frame.origin.y + projected.Value()->viewportPosition.y * frame.size.y};
    }
}  // namespace Horo::Editor
