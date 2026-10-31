#include "TerrainSculptProducer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Stable integer mixing gives deterministic signed noise without ambient RNG state. */
        float Noise(const std::uint32_t x, const std::uint32_t z, const std::uint64_t seed) noexcept {
            std::uint64_t value = seed ^ (std::uint64_t{x} << 32U) ^ z;
            value += 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
            value ^= value >> 31U;
            return static_cast<float>(value >> 40U) / 8'388'607.5F - 1.0F;
        }

        /** @brief Rejects unsupported brush state rather than substituting another operation. */
        bool ValidBrush(const TerrainSculptBrush &brush) noexcept {
            return brush.mode <= TerrainSculptMode::Noise && std::isfinite(brush.radiusMeters) && brush.radiusMeters > 0 &&
                   std::isfinite(brush.amountMeters) && brush.amountMeters >= 0 && std::isfinite(brush.blend) && brush.blend >= 0 &&
                   brush.blend <= 1 && std::isfinite(brush.flattenMeters);
        }

        /** @brief Clip floating sample bounds before conversion to a bounded grid index. */
        std::uint32_t Index(const double sample, const std::uint32_t maximum, const bool upper) noexcept {
            return static_cast<std::uint32_t>(std::clamp(upper ? std::ceil(sample) : std::floor(sample), 0.0, double(maximum)));
        }
    }  // namespace

    /** @copydoc ProduceTerrainSculpt */
    Result<TerrainEditOperation> ProduceTerrainSculpt(const Terrain::TerrainCanonicalSource &source, TerrainEditFence fence,
                                                      TerrainEditOperationId operation, const TerrainSculptBrush &brush,
                                                      std::span<const TerrainSculptStamp> stamps, const TerrainSculptLimits &limits,
                                                      const CancellationToken &cancellation) {
        using Return = Result<TerrainEditOperation>;
        if (cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(TerrainEditErrors::Cancelled));
        if (source.revision != fence.expectedRevision || source.dataset != fence.dataset || source.capability != fence.capabilityRevision)
            return Return::Failure(MakeError(TerrainEditErrors::StaleRevision));
        if (!operation.IsValid() || !fence.session.IsValid() || !ValidBrush(brush) || source.width < 2 || source.height < 2 ||
            source.width > Terrain::TerrainDescriptorHardLimits::SamplesPerAxis ||
            source.height > Terrain::TerrainDescriptorHardLimits::SamplesPerAxis ||
            source.heightsMeters.size() != std::uint64_t{source.width} * source.height || !std::isfinite(source.coordinates.spacingX) ||
            source.coordinates.spacingX <= 0 || !std::isfinite(source.coordinates.spacingZ) || source.coordinates.spacingZ <= 0 ||
            stamps.empty())
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        if (limits.maximumStamps == 0 || limits.maximumPatchSamples == 0 || limits.maximumSampleVisits == 0 ||
            stamps.size() > limits.maximumStamps)
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        const double rx = brush.radiusMeters / source.coordinates.spacingX;
        const double rz = brush.radiusMeters / source.coordinates.spacingZ;
        if (!std::isfinite(rx) || !std::isfinite(rz))
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        std::uint32_t minX = source.width - 1, minZ = source.height - 1, maxX{}, maxZ{};
        for (const auto &stamp : stamps) {
            if (!std::isfinite(stamp.sampleX) || !std::isfinite(stamp.sampleZ) || stamp.sampleX < 0 || stamp.sampleX > source.width - 1 ||
                stamp.sampleZ < 0 || stamp.sampleZ > source.height - 1)
                return Return::Failure(MakeError(TerrainEditErrors::Invalid));
            minX = std::min(minX, Index(stamp.sampleX - rx, source.width - 1, false));
            minZ = std::min(minZ, Index(stamp.sampleZ - rz, source.height - 1, false));
            maxX = std::max(maxX, Index(stamp.sampleX + rx, source.width - 1, true));
            maxZ = std::max(maxZ, Index(stamp.sampleZ + rz, source.height - 1, true));
        }
        const std::uint32_t width = maxX - minX + 1, height = maxZ - minZ + 1;
        const std::uint64_t samples = std::uint64_t{width} * height;
        // Smooth visits at most nine canonical neighbors plus the center/output per stamp.
        const std::uint64_t visits = brush.mode == TerrainSculptMode::Smooth ? 11U : 2U;
        if (samples > limits.maximumPatchSamples || stamps.size() > limits.maximumSampleVisits / visits / samples)
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        try {
            TerrainRasterPatch patch{.rect = {minX, minZ, width, height}};
            patch.heightsMeters.resize(static_cast<std::size_t>(samples));
            for (std::uint32_t z = 0; z < height; ++z)
                for (std::uint32_t x = 0; x < width; ++x)
                    patch.heightsMeters[std::size_t{z} * width + x] = source.heightsMeters[std::size_t{z + minZ} * source.width + x + minX];
            std::vector<float> previous;
            if (brush.mode == TerrainSculptMode::Smooth)
                previous.resize(patch.heightsMeters.size());
            for (const auto &stamp : stamps) {
                if (cancellation.IsCancellationRequested())
                    return Return::Failure(MakeError(TerrainEditErrors::Cancelled));
                if (!previous.empty())
                    std::copy(patch.heightsMeters.begin(), patch.heightsMeters.end(), previous.begin());
                for (std::uint32_t z = minZ; z <= maxZ; ++z) {
                    if (cancellation.IsCancellationRequested())
                        return Return::Failure(MakeError(TerrainEditErrors::Cancelled));
                    for (std::uint32_t x = minX; x <= maxX; ++x) {
                        const double distance = std::hypot((x - stamp.sampleX) * source.coordinates.spacingX,
                                                           (z - stamp.sampleZ) * source.coordinates.spacingZ);
                        if (distance >= brush.radiusMeters)
                            continue;
                        const auto index = std::size_t{z - minZ} * width + x - minX;
                        const float before = patch.heightsMeters[index];
                        if (!std::isfinite(before))
                            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
                        const float t = static_cast<float>(1.0 - distance / brush.radiusMeters);
                        const float falloff = t * t * (3.0F - 2.0F * t);
                        double value = before;
                        switch (brush.mode) {
                            case TerrainSculptMode::Raise:
                                value += double(brush.amountMeters) * falloff;
                                break;
                            case TerrainSculptMode::Lower:
                                value -= double(brush.amountMeters) * falloff;
                                break;
                            case TerrainSculptMode::Noise:
                                value += double(brush.amountMeters) * falloff * Noise(x, z, brush.noiseSeed);
                                break;
                            case TerrainSculptMode::Flatten:
                                value = std::lerp(double(before), double(brush.flattenMeters), double(brush.blend * falloff));
                                break;
                            case TerrainSculptMode::Smooth: {
                                double sum{};
                                std::uint32_t count{};
                                for (std::uint32_t nz = z == 0 ? 0 : z - 1; nz <= std::min(z + 1, source.height - 1); ++nz)
                                    for (std::uint32_t nx = x == 0 ? 0 : x - 1; nx <= std::min(x + 1, source.width - 1); ++nx) {
                                        const float sample = nx >= minX && nx <= maxX && nz >= minZ && nz <= maxZ
                                                                 ? previous[std::size_t{nz - minZ} * width + nx - minX]
                                                                 : source.heightsMeters[std::size_t{nz} * source.width + nx];
                                        if (!std::isfinite(sample))
                                            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
                                        sum += sample;
                                        ++count;
                                    }
                                value = std::lerp(double(before), sum / count, double(brush.blend * falloff));
                                break;
                            }
                        }
                        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
                        patch.heightsMeters[index] = static_cast<float>(value);
                    }
                }
            }
            TerrainEditOperation result{.fence = fence, .operation = operation};
            result.patches.push_back(std::move(patch));
            return Return::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        }
    }
}  // namespace Horo::Editor
