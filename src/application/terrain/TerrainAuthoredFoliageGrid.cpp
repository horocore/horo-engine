#include "TerrainAuthoredFoliageGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Horo::Application::TerrainCookDetail {
    namespace {
        /** @brief Round only inside the representable integer interval; preserve the source's explicit precision tolerance. */
        Result<std::int64_t> Millimeters(double meters, double tolerance) {
            const double scaled = meters * 1000;
            constexpr auto maximum = static_cast<double>(std::numeric_limits<std::int64_t>::max());
            if (!std::isfinite(scaled) || scaled >= maximum || scaled <= -maximum)
                return Result<std::int64_t>::Failure(MakeError(Terrain::TerrainSourceErrors::PrecisionLost));
            const auto rounded = std::round(scaled);
            if (std::abs(rounded / 1000 - meters) > tolerance)
                return Result<std::int64_t>::Failure(MakeError(Terrain::TerrainSourceErrors::PrecisionLost));
            return Result<std::int64_t>::Success(static_cast<std::int64_t>(rounded));
        }

        Result<std::uint32_t> Spacing(double spacing, double tolerance) {
            auto integer = Millimeters(spacing, tolerance);
            if (integer.HasError()) return Result<std::uint32_t>::Failure(std::move(integer).ErrorValue());
            if (integer.Value() <= 0 || static_cast<std::uint64_t>(integer.Value()) > std::numeric_limits<std::uint32_t>::max())
                return Result<std::uint32_t>::Failure(MakeError(Terrain::TerrainSourceErrors::PrecisionLost));
            return Result<std::uint32_t>::Success(static_cast<std::uint32_t>(integer.Value()));
        }

        struct Axis final { std::int64_t origin; std::uint32_t spacing; };

        /** @brief The affine far edge also fits and meets tolerance; rounding one spacing must not accumulate hidden drift. */
        Result<Axis> AxisGrid(double origin, double spacing, std::uint32_t samples, double tolerance) {
            using Return = Result<Axis>;
            auto first = Millimeters(origin, tolerance);
            if (first.HasError()) return Return::Failure(std::move(first).ErrorValue());
            auto step = Spacing(spacing, tolerance);
            if (step.HasError()) return Return::Failure(std::move(step).ErrorValue());
            auto last = Millimeters(origin + double(samples - 1) * spacing, tolerance);
            if (last.HasError()) return Return::Failure(std::move(last).ErrorValue());
            const auto delta = std::uint64_t{samples - 1} * step.Value();
            if (delta > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                first.Value() > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(delta))
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::PrecisionLost));
            const auto final = first.Value() + static_cast<std::int64_t>(delta);
            if (std::abs(double(final) / 1000 - (origin + double(samples - 1) * spacing)) > tolerance)
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::PrecisionLost));
            return Return::Success({first.Value(), step.Value()});
        }

        /** @brief Canonical visual/collision/navigation geometry omits a quad when any of its closed samples is a hole. */
        bool Hole(const Terrain::TerrainCanonicalSource &source, std::uint32_t x, std::uint32_t z) noexcept {
            if (!source.HasHoles()) return false;
            const auto a = std::size_t(z) * source.width + x;
            return source.holes[a] || source.holes[a + 1] || source.holes[a + source.width] || source.holes[a + source.width + 1];
        }

        /** @brief Physical upward normal of the canonical {a,c,b} triangle at the selected integer sample's adjacent cell. */
        Result<void> Normal(const Terrain::TerrainCanonicalSource &source, std::uint32_t x, std::uint32_t z,
                              Terrain::FoliagePlacementSample &sample) {
            const auto a = std::size_t(z) * source.width + x;
            const double gx = (double(source.heightsMeters[a + 1]) - source.heightsMeters[a]) / source.coordinates.spacingX;
            const double gz = (double(source.heightsMeters[a + source.width]) - source.heightsMeters[a]) / source.coordinates.spacingZ;
            const auto length = std::hypot(gx, 1.0, gz);
            if (!std::isfinite(length) || length <= 0)
                return Result<void>::Failure(MakeError(Terrain::TerrainSourceErrors::PrecisionLost));
            const double y = 1.0 / length;
            sample.normalXPermille = static_cast<std::int16_t>(std::round(-gx / length * 1000));
            sample.normalYPermille = static_cast<std::int16_t>(std::round(y * 1000));
            sample.normalZPermille = static_cast<std::int16_t>(std::round(-gz / length * 1000));
            sample.slopeMilliDegrees = static_cast<std::uint32_t>(std::round(std::acos(std::clamp(y, 0.0, 1.0)) * 180000 / std::numbers::pi));
            if (sample.normalYPermille <= 0)
                return Result<void>::Failure(MakeError(Terrain::TerrainSourceErrors::PrecisionLost));
            return Result<void>::Success();
        }

        Result<Terrain::FoliagePlacementSample> Sample(const Terrain::TerrainCanonicalSource &source,
                                                       std::uint32_t x, std::uint32_t z) {
            using Return = Result<Terrain::FoliagePlacementSample>;
            auto height = Millimeters(source.heightsMeters[std::size_t(z) * source.width + x], source.coordinates.maximumPrecisionError);
            if (height.HasError()) return Return::Failure(std::move(height).ErrorValue());
            const auto cellX = std::min(x, source.width - 2), cellZ = std::min(z, source.height - 2);
            Terrain::FoliagePlacementSample sample;
            sample.altitudeMillimeters = height.Value();
            sample.density = 65535;
            sample.hole = Hole(source, cellX, cellZ);
            if (auto normal = Normal(source, cellX, cellZ, sample); normal.HasError())
                return Return::Failure(std::move(normal).ErrorValue());
            return Return::Success(sample);
        }

        Result<void> Fill(FoliageTileGrid &grid, const Terrain::TerrainCanonicalSource &source,
                           const FoliageTileRegion &region, const CancellationToken &cancellation) {
            grid.samples.reserve(std::size_t(region.width) * region.height);
            for (std::uint32_t z = 0; z < region.height; ++z) {
                if (cancellation.IsCancellationRequested()) return Result<void>::Failure(MakeError(Terrain::FoliagePlacementCookErrors::Cancelled));
                for (std::uint32_t x = 0; x < region.width; ++x) {
                    auto sample = Sample(source, region.x + x, region.z + z);
                    if (sample.HasError()) return Result<void>::Failure(std::move(sample).ErrorValue());
                    grid.samples.push_back(sample.Value());
                }
            }
            return Result<void>::Success();
        }
    }

    /** @copydoc BuildFoliageGrid */
    Result<FoliageTileGrid> BuildFoliageGrid(const Terrain::TerrainCanonicalSource &source,
        const FoliageTileRegion &region, const CancellationToken &cancellation) {
        using Return = Result<FoliageTileGrid>;
        auto x = AxisGrid(source.coordinates.originX + region.x * source.coordinates.spacingX,
            source.coordinates.spacingX, region.width, source.coordinates.maximumPrecisionError);
        if (x.HasError()) return Return::Failure(std::move(x).ErrorValue());
        auto z = AxisGrid(source.coordinates.originZ + region.z * source.coordinates.spacingZ,
            source.coordinates.spacingZ, region.height, source.coordinates.maximumPrecisionError);
        if (z.HasError()) return Return::Failure(std::move(z).ErrorValue());
        FoliageTileGrid grid;
        if (auto filled = Fill(grid, source, region, cancellation); filled.HasError()) return Return::Failure(std::move(filled).ErrorValue());
        grid.view = {.tile = region.tile, .sourceRevision = source.revision, .capability = source.capability,
            .originXMillimeters = x.Value().origin, .originZMillimeters = z.Value().origin,
            .spacingXMillimeters = x.Value().spacing, .spacingZMillimeters = z.Value().spacing,
            .width = region.width, .height = region.height, .samples = grid.samples};
        return Return::Success(std::move(grid));
    }
}
