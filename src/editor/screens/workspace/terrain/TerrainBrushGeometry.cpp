#include "TerrainBrushGeometry.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    namespace {
        bool ValidDimensions(const Terrain::TerrainCanonicalSource &source) noexcept {
            return source.width >= 2 && source.height >= 2 && source.width <= Terrain::TerrainDescriptorHardLimits::SamplesPerAxis &&
                   source.height <= Terrain::TerrainDescriptorHardLimits::SamplesPerAxis &&
                   source.heightsMeters.size() == std::uint64_t{source.width} * source.height;
        }

        bool ValidScale(const Terrain::TerrainCanonicalSource &source, double radius) noexcept {
            return std::isfinite(radius) && radius > 0 && std::isfinite(source.coordinates.spacingX) && source.coordinates.spacingX > 0 &&
                   std::isfinite(source.coordinates.spacingZ) && source.coordinates.spacingZ > 0;
        }

        bool ValidLimits(const TerrainSculptLimits &limits) noexcept {
            return limits.maximumStamps > 0 && limits.maximumStamps <= 64 && limits.maximumPatchSamples > 0 &&
                   limits.maximumPatchSamples <= 16'384 && limits.maximumSampleVisits > 0 &&
                   limits.maximumSampleVisits <= Terrain::TerrainDescriptorHardLimits::WorkItems;
        }

        bool ValidStamp(const TerrainSculptStamp &stamp, const Terrain::TerrainCanonicalSource &source) noexcept {
            return std::isfinite(stamp.sampleX) && std::isfinite(stamp.sampleZ) && stamp.sampleX >= 0 && stamp.sampleZ >= 0 &&
                   stamp.sampleX <= source.width - 1 && stamp.sampleZ <= source.height - 1;
        }

        std::uint32_t Bound(double sample, std::uint32_t maximum, bool upper) noexcept {
            return static_cast<std::uint32_t>(std::clamp(upper ? std::ceil(sample) : std::floor(sample), 0.0, double(maximum)));
        }

        Result<TerrainPatchRect> Bounds(const Terrain::TerrainCanonicalSource &source, double radius,
                                        std::span<const TerrainSculptStamp> stamps) {
            const double rx = radius / source.coordinates.spacingX;
            const double rz = radius / source.coordinates.spacingZ;
            if (!std::isfinite(rx) || !std::isfinite(rz))
                return Result<TerrainPatchRect>::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            std::uint32_t minX = source.width - 1;
            std::uint32_t minZ = source.height - 1;
            std::uint32_t maxX{};
            std::uint32_t maxZ{};
            for (const auto &stamp : stamps) {
                if (!ValidStamp(stamp, source))
                    return Result<TerrainPatchRect>::Failure(MakeError(TerrainEditErrors::Invalid));
                minX = std::min(minX, Bound(stamp.sampleX - rx, source.width - 1, false));
                minZ = std::min(minZ, Bound(stamp.sampleZ - rz, source.height - 1, false));
                maxX = std::max(maxX, Bound(stamp.sampleX + rx, source.width - 1, true));
                maxZ = std::max(maxZ, Bound(stamp.sampleZ + rz, source.height - 1, true));
            }
            return Result<TerrainPatchRect>::Success({minX, minZ, maxX - minX + 1, maxZ - minZ + 1});
        }

        Result<std::uint64_t> Charge(std::uint64_t samples, std::size_t stamps, const TerrainSculptLimits &limits, std::uint64_t setup,
                                     std::uint64_t perStamp, std::uint64_t fixed) {
            using Return = Result<std::uint64_t>;
            const auto maximum = std::uint64_t{limits.maximumSampleVisits};
            const auto geometry = stamps * 24U;
            if (samples > limits.maximumPatchSamples || geometry > maximum || fixed > maximum - geometry || perStamp == 0)
                return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            const auto remaining = maximum - fixed - geometry;
            if (setup > remaining / samples)
                return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            const auto setupTotal = setup * samples;
            if (perStamp > (remaining - setupTotal) / samples / stamps)
                return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            return Return::Success(fixed + geometry + setupTotal + samples * stamps * perStamp);
        }
    }  // namespace

    /** @copydoc PrepareTerrainBrushFootprint */
    Result<TerrainBrushFootprint> PrepareTerrainBrushFootprint(const Terrain::TerrainCanonicalSource &source, double radius,
                                                               std::span<const TerrainSculptStamp> stamps,
                                                               const TerrainSculptLimits &limits, std::uint64_t setup,
                                                               std::uint64_t perStamp, std::uint64_t fixed) {
        using Return = Result<TerrainBrushFootprint>;
        if (!ValidDimensions(source) || !ValidScale(source, radius) || stamps.empty())
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        if (!ValidLimits(limits) || stamps.size() > limits.maximumStamps)
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        auto rect = Bounds(source, radius, stamps);
        if (rect.HasError())
            return Return::Failure(std::move(rect).ErrorValue());
        const auto samples = std::uint64_t{rect.Value().width} * rect.Value().height;
        auto charged = Charge(samples, stamps.size(), limits, setup, perStamp, fixed);
        if (charged.HasError())
            return Return::Failure(std::move(charged).ErrorValue());
        return Return::Success({rect.Value(), static_cast<std::size_t>(samples), charged.Value()});
    }
}  // namespace Horo::Editor
