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

        bool ValidStrength(const TerrainSculptBrush &brush) noexcept {
            return std::isfinite(brush.amountMeters) && brush.amountMeters >= 0 && std::isfinite(brush.blend) && brush.blend >= 0 &&
                   brush.blend <= 1;
        }

        /** @brief Rejects unsupported brush state rather than substituting another operation. */
        bool ValidBrush(const TerrainSculptBrush &brush) noexcept {
            return brush.mode <= TerrainSculptMode::Noise && std::isfinite(brush.radiusMeters) && brush.radiusMeters > 0 &&
                   ValidStrength(brush) && std::isfinite(brush.flattenMeters);
        }

        /** @brief Clip floating sample bounds before conversion to a bounded grid index. */
        std::uint32_t Index(const double sample, const std::uint32_t maximum, const bool upper) noexcept {
            return static_cast<std::uint32_t>(std::clamp(upper ? std::ceil(sample) : std::floor(sample), 0.0, double(maximum)));
        }

        bool ValidGrid(const Terrain::TerrainCanonicalSource &source) noexcept {
            return source.width >= 2 && source.height >= 2 && source.width <= Terrain::TerrainDescriptorHardLimits::SamplesPerAxis &&
                   source.height <= Terrain::TerrainDescriptorHardLimits::SamplesPerAxis &&
                   source.heightsMeters.size() == std::uint64_t{source.width} * source.height;
        }

        bool ValidSpacing(const Terrain::TerrainSourceCoordinates &coordinates) noexcept {
            return std::isfinite(coordinates.spacingX) && coordinates.spacingX > 0 && std::isfinite(coordinates.spacingZ) &&
                   coordinates.spacingZ > 0;
        }

        bool MatchesFence(const Terrain::TerrainCanonicalSource &source, const TerrainEditFence &fence) noexcept {
            return source.revision == fence.expectedRevision && source.dataset == fence.dataset &&
                   source.capability == fence.capabilityRevision;
        }

        Result<void> AdmitSource(const Terrain::TerrainCanonicalSource &source, const TerrainEditFence &fence,
                                 TerrainEditOperationId operation, const TerrainSculptBrush &brush,
                                 std::span<const TerrainSculptStamp> stamps, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
            if (!MatchesFence(source, fence))
                return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
            if (!operation.IsValid() || !fence.session.IsValid() || !ValidBrush(brush) || !ValidGrid(source) ||
                !ValidSpacing(source.coordinates) || stamps.empty())
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            return Result<void>::Success();
        }

        bool ValidStamp(const TerrainSculptStamp &stamp, const Terrain::TerrainCanonicalSource &source) noexcept {
            return std::isfinite(stamp.sampleX) && std::isfinite(stamp.sampleZ) && stamp.sampleX >= 0 &&
                   stamp.sampleX <= source.width - 1 && stamp.sampleZ >= 0 && stamp.sampleZ <= source.height - 1;
        }

        struct SculptRegion final {
            std::uint32_t minX;
            std::uint32_t minZ;
            std::uint32_t maxX;
            std::uint32_t maxZ;

            std::uint32_t Width() const noexcept {
                return maxX - minX + 1;
            }

            std::uint32_t Height() const noexcept {
                return maxZ - minZ + 1;
            }

            std::uint64_t Samples() const noexcept {
                return std::uint64_t{Width()} * Height();
            }

            std::size_t Offset(std::uint32_t x, std::uint32_t z) const noexcept {
                return std::size_t{z - minZ} * Width() + x - minX;
            }

            bool Contains(std::uint32_t x, std::uint32_t z) const noexcept {
                return x >= minX && x <= maxX && z >= minZ && z <= maxZ;
            }
        };

        Result<SculptRegion> CaptureRegion(const Terrain::TerrainCanonicalSource &source, const TerrainSculptBrush &brush,
                                           std::span<const TerrainSculptStamp> stamps, const TerrainSculptLimits &limits) {
            using Return = Result<SculptRegion>;
            if (limits.maximumStamps == 0 || limits.maximumPatchSamples == 0 || limits.maximumSampleVisits == 0 ||
                stamps.size() > limits.maximumStamps)
                return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            const double rx = brush.radiusMeters / source.coordinates.spacingX;
            const double rz = brush.radiusMeters / source.coordinates.spacingZ;
            if (!std::isfinite(rx) || !std::isfinite(rz))
                return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            SculptRegion region{source.width - 1, source.height - 1, 0, 0};
            for (const auto &stamp : stamps) {
                if (!ValidStamp(stamp, source))
                    return Return::Failure(MakeError(TerrainEditErrors::Invalid));
                region.minX = std::min(region.minX, Index(stamp.sampleX - rx, source.width - 1, false));
                region.minZ = std::min(region.minZ, Index(stamp.sampleZ - rz, source.height - 1, false));
                region.maxX = std::max(region.maxX, Index(stamp.sampleX + rx, source.width - 1, true));
                region.maxZ = std::max(region.maxZ, Index(stamp.sampleZ + rz, source.height - 1, true));
            }
            return Return::Success(region);
        }

        Result<void> AdmitWork(const SculptRegion &region, const TerrainSculptBrush &brush, std::size_t stampCount,
                               const TerrainSculptLimits &limits) {
            // Smooth visits at most nine canonical neighbors plus the center/output per stamp.
            if (const std::uint64_t visits = brush.mode == TerrainSculptMode::Smooth ? 11U : 2U;
                region.Samples() > limits.maximumPatchSamples || stampCount > limits.maximumSampleVisits / visits / region.Samples())
                return Result<void>::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            return Result<void>::Success();
        }

        TerrainRasterPatch CopyPatch(const Terrain::TerrainCanonicalSource &source, const SculptRegion &region) {
            TerrainRasterPatch patch{.rect = {region.minX, region.minZ, region.Width(), region.Height()}};
            patch.heightsMeters.resize(static_cast<std::size_t>(region.Samples()));
            for (std::uint32_t z = region.minZ; z <= region.maxZ; ++z)
                for (std::uint32_t x = region.minX; x <= region.maxX; ++x)
                    patch.heightsMeters[region.Offset(x, z)] = source.heightsMeters[std::size_t{z} * source.width + x];
            return patch;
        }

        /** @brief Read the previous stamp inside the patch and unchanged canonical samples outside it. */
        Result<double> NeighborAverage(const Terrain::TerrainCanonicalSource &source, const SculptRegion &region,
                                       std::span<const float> previous, std::uint32_t x, std::uint32_t z) {
            double sum{};
            std::uint32_t count{};
            for (std::uint32_t nz = z == 0 ? 0 : z - 1; nz <= std::min(z + 1, source.height - 1); ++nz)
                for (std::uint32_t nx = x == 0 ? 0 : x - 1; nx <= std::min(x + 1, source.width - 1); ++nx) {
                    const float sample = region.Contains(nx, nz) ? previous[region.Offset(nx, nz)]
                                                                 : source.heightsMeters[std::size_t{nz} * source.width + nx];
                    if (!std::isfinite(sample))
                        return Result<double>::Failure(MakeError(TerrainEditErrors::Invalid));
                    sum += sample;
                    ++count;
                }
            return Result<double>::Success(sum / count);
        }

        Result<double> SculptHeight(const Terrain::TerrainCanonicalSource &source, const SculptRegion &region,
                                    const TerrainSculptBrush &brush, std::span<const float> previous, std::uint32_t x, std::uint32_t z,
                                    float before, float falloff) {
            switch (brush.mode) {
                case TerrainSculptMode::Raise:
                    return Result<double>::Success(double(before) + double(brush.amountMeters) * falloff);
                case TerrainSculptMode::Lower:
                    return Result<double>::Success(double(before) - double(brush.amountMeters) * falloff);
                case TerrainSculptMode::Noise:
                    return Result<double>::Success(double(before) + double(brush.amountMeters) * falloff * Noise(x, z, brush.noiseSeed));
                case TerrainSculptMode::Flatten:
                    return Result<double>::Success(std::lerp(double(before), double(brush.flattenMeters), double(brush.blend * falloff)));
                case TerrainSculptMode::Smooth: {
                    auto average = NeighborAverage(source, region, previous, x, z);
                    if (average.HasError())
                        return Result<double>::Failure(std::move(average).ErrorValue());
                    return Result<double>::Success(std::lerp(double(before), average.Value(), double(brush.blend * falloff)));
                }
            }
            return Result<double>::Failure(MakeError(TerrainEditErrors::Invalid));
        }

        Result<void> ApplySample(const Terrain::TerrainCanonicalSource &source, const SculptRegion &region, const TerrainSculptBrush &brush,
                                 const TerrainSculptStamp &stamp, std::span<const float> previous, TerrainRasterPatch &patch,
                                 std::uint32_t x, std::uint32_t z) {
            const double distance =
                std::hypot((x - stamp.sampleX) * source.coordinates.spacingX, (z - stamp.sampleZ) * source.coordinates.spacingZ);
            if (distance >= brush.radiusMeters)
                return Result<void>::Success();
            const auto index = region.Offset(x, z);
            const float before = patch.heightsMeters[index];
            if (!std::isfinite(before))
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            const float t = static_cast<float>(1.0 - distance / brush.radiusMeters);
            const float falloff = t * t * (3.0F - 2.0F * t);
            auto value = SculptHeight(source, region, brush, previous, x, z, before, falloff);
            if (value.HasError())
                return Result<void>::Failure(std::move(value).ErrorValue());
            if (!std::isfinite(value.Value()) || std::abs(value.Value()) > std::numeric_limits<float>::max())
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            patch.heightsMeters[index] = static_cast<float>(value.Value());
            return Result<void>::Success();
        }

        Result<void> ApplyStamp(const Terrain::TerrainCanonicalSource &source, const SculptRegion &region, const TerrainSculptBrush &brush,
                                const TerrainSculptStamp &stamp, std::span<const float> previous, TerrainRasterPatch &patch,
                                const CancellationToken &cancellation) {
            for (std::uint32_t z = region.minZ; z <= region.maxZ; ++z) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
                for (std::uint32_t x = region.minX; x <= region.maxX; ++x) {
                    auto applied = ApplySample(source, region, brush, stamp, previous, patch, x, z);
                    if (applied.HasError())
                        return applied;
                }
            }
            return Result<void>::Success();
        }

        Result<TerrainRasterPatch> BuildPatch(const Terrain::TerrainCanonicalSource &source, const SculptRegion &region,
                                              const TerrainSculptBrush &brush, std::span<const TerrainSculptStamp> stamps,
                                              const CancellationToken &cancellation) {
            auto patch = CopyPatch(source, region);
            std::vector<float> previous;
            if (brush.mode == TerrainSculptMode::Smooth)
                previous.resize(patch.heightsMeters.size());
            for (const auto &stamp : stamps) {
                if (cancellation.IsCancellationRequested())
                    return Result<TerrainRasterPatch>::Failure(MakeError(TerrainEditErrors::Cancelled));
                if (!previous.empty())
                    std::copy(patch.heightsMeters.begin(), patch.heightsMeters.end(), previous.begin());
                auto applied = ApplyStamp(source, region, brush, stamp, previous, patch, cancellation);
                if (applied.HasError())
                    return Result<TerrainRasterPatch>::Failure(std::move(applied).ErrorValue());
            }
            return Result<TerrainRasterPatch>::Success(std::move(patch));
        }
    }  // namespace

    /** @copydoc ProduceTerrainSculpt */
    Result<TerrainEditOperation> ProduceTerrainSculpt(const Terrain::TerrainCanonicalSource &source, TerrainEditFence fence,
                                                      TerrainEditOperationId operation, const TerrainSculptBrush &brush,
                                                      std::span<const TerrainSculptStamp> stamps, const TerrainSculptLimits &limits,
                                                      const CancellationToken &cancellation) {
        using Return = Result<TerrainEditOperation>;
        if (auto admitted = AdmitSource(source, fence, operation, brush, stamps, cancellation); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        auto region = CaptureRegion(source, brush, stamps, limits);
        if (region.HasError())
            return Return::Failure(std::move(region).ErrorValue());
        if (auto work = AdmitWork(region.Value(), brush, stamps.size(), limits); work.HasError())
            return Return::Failure(std::move(work).ErrorValue());
        try {
            auto patch = BuildPatch(source, region.Value(), brush, stamps, cancellation);
            if (patch.HasError())
                return Return::Failure(std::move(patch).ErrorValue());
            TerrainEditOperation result{.fence = fence, .operation = operation};
            result.patches.push_back(std::move(patch).Value());
            return Return::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        }
    }
}  // namespace Horo::Editor
