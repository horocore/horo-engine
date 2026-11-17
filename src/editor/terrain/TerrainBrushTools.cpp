#include "Horo/Editor/TerrainBrushTools.h"

#include "TerrainBrushAdmission.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Translates planner admission without changing document state. */
        Result<TerrainEditOperation> Failed(const ErrorCodeDescriptor &error) {
            return Result<TerrainEditOperation>::Failure(MakeError(error));
        }
    }

    /** @copydoc PlanTerrainBrush */
    Result<TerrainEditOperation> PlanTerrainBrush(const TerrainAuthoringDocument &document, const TerrainBrushSettings &settings,
        std::span<const TerrainBrushDab> dabs, TerrainEditOperationId operation, const TerrainBrushLimits &limits,
        const CancellationToken &cancellation) {
        if (document.IsClosed()) return Failed(TerrainEditErrors::Closed);
        if (cancellation.IsCancellationRequested()) return Failed(TerrainEditErrors::Cancelled);
        if (!operation.IsValid() || dabs.empty() || !std::isfinite(settings.radiusMeters) || settings.radiusMeters <= 0 ||
            !std::isfinite(settings.strength) || settings.strength < 0 || settings.mode != TerrainBrushMode::Raise)
            return Failed(TerrainEditErrors::Invalid);
        if (dabs.size() > limits.maximumDabs) return Failed(TerrainEditErrors::LimitExceeded);
        const auto &source = document.Source();
        const double rx = settings.radiusMeters / source.coordinates.spacingX;
        const double rz = settings.radiusMeters / source.coordinates.spacingZ;
        double x0 = source.width - 1.0, z0 = source.height - 1.0, x1 = 0, z1 = 0;
        for (const auto &dab : dabs) {
            if (!std::isfinite(dab.x) || !std::isfinite(dab.z) || dab.x < 0 || dab.z < 0 ||
                dab.x > source.width - 1.0 || dab.z > source.height - 1.0)
                return Failed(TerrainEditErrors::Invalid);
            x0 = std::min(x0, std::max(0.0, std::ceil(dab.x - rx)));
            z0 = std::min(z0, std::max(0.0, std::ceil(dab.z - rz)));
            x1 = std::max(x1, std::min(source.width - 1.0, std::floor(dab.x + rx)));
            z1 = std::max(z1, std::min(source.height - 1.0, std::floor(dab.z + rz)));
        }
        if (x0 > x1 || z0 > z1) return Failed(TerrainEditErrors::Invalid);
        const TerrainPatchRect rect{static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(z0),
            static_cast<std::uint32_t>(x1 - x0 + 1), static_cast<std::uint32_t>(z1 - z0 + 1)};
        const std::uint64_t count = static_cast<std::uint64_t>(rect.width) * rect.height;
        if (count > limits.maximumSamples || !Detail::CanOwnTerrainHeightPatch(count, limits.maximumOwnedBytes) ||
            count > limits.maximumReadVisits / (dabs.size() + 1)) return Failed(TerrainEditErrors::LimitExceeded);
        try {
            TerrainRasterPatch patch{.rect = rect};
            patch.heightsMeters.reserve(static_cast<std::size_t>(count));
            for (std::uint32_t z = rect.z; z < rect.z + rect.height; ++z)
                for (std::uint32_t x = rect.x; x < rect.x + rect.width; ++x)
                    patch.heightsMeters.push_back(source.heightsMeters[static_cast<std::size_t>(z) * source.width + x]);
            for (const auto &dab : dabs) {
                if (cancellation.IsCancellationRequested()) return Failed(TerrainEditErrors::Cancelled);
                for (std::uint32_t z = 0; z < rect.height; ++z)
                    for (std::uint32_t x = 0; x < rect.width; ++x) {
                        const double dx = (rect.x + x - dab.x) * source.coordinates.spacingX;
                        const double dz = (rect.z + z - dab.z) * source.coordinates.spacingZ;
                        const double falloff = std::max(0.0, 1.0 - std::hypot(dx, dz) / settings.radiusMeters);
                        float &height = patch.heightsMeters[static_cast<std::size_t>(z) * rect.width + x];
                        const double changed = height + settings.strength * falloff;
                        if (!Detail::TryNarrowTerrainHeight(changed, height)) return Failed(TerrainEditErrors::Invalid);
                    }
            }
            TerrainEditOperation result{.fence = document.Fence(), .operation = operation};
            result.patches.push_back(std::move(patch));
            return Result<TerrainEditOperation>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Failed(TerrainEditErrors::LimitExceeded);
        }
    }
}
