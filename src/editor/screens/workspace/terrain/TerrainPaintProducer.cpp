#include "TerrainPaintProducer.h"

#include "TerrainBrushGeometry.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <numeric>

namespace Horo::Editor {
    namespace {
        using Binding = Terrain::TerrainAuthoringLayerBinding;
        using Weights = std::array<std::uint16_t, Terrain::TerrainDescriptorHardLimits::LayersPerTile>;
        using RawWeights = std::array<std::uint32_t, Terrain::TerrainDescriptorHardLimits::LayersPerTile>;

        struct PaintPlan final {
            TerrainBrushFootprint footprint;
            Weights desired{};
        };

        bool SameDocument(const Terrain::TerrainCanonicalSource &source, const TerrainEditFence &fence) noexcept {
            return fence.session.IsValid() && source.dataset == fence.dataset && source.revision == fence.expectedRevision &&
                   source.capability == fence.capabilityRevision;
        }

        bool SameLayers(const std::optional<Binding> &binding, const TerrainPaintFence &fence) noexcept {
            if (!binding)
                return !fence.layerSet.IsValid() && !fence.layerRevision.IsValid();
            return binding->id == fence.layerSet && binding->revision == fence.layerRevision && binding->digest == fence.layerDigest;
        }

        bool ValidBrush(const TerrainPaintBrush &brush) noexcept {
            return brush.mode <= TerrainPaintMode::RestoreHole && std::isfinite(brush.strength) && brush.strength >= 0 &&
                   brush.strength <= 1 && brush.targetCount <= brush.targets.size();
        }

        Result<void> AdmitChannels(const Terrain::TerrainCanonicalSource &source, const std::optional<Binding> &binding,
                                   TerrainPaintMode mode) {
            const auto samples = std::uint64_t{source.width} * source.height;
            if (mode != TerrainPaintMode::Weights) {
                if (source.holes.size() != samples)
                    return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
                return Result<void>::Success();
            }
            if (!binding || source.layerCount == 0 || source.layerCount > Terrain::TerrainDescriptorHardLimits::LayersPerTile)
                return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            if (source.weights.size() != samples * source.layerCount)
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            return Result<void>::Success();
        }

        /** @brief Resolve a semantic identity without treating a palette slot as stored channel identity. */
        Result<std::size_t> LayerIndex(const Binding &binding, std::uint8_t count, Terrain::TerrainLayerId target) {
            using Return = Result<std::size_t>;
            std::optional<std::size_t> found;
            for (std::size_t i = 0; i < count; ++i) {
                if (binding.order[i] != target)
                    continue;
                if (found.has_value())
                    return Return::Failure(MakeError(TerrainEditErrors::Invalid));
                found = i;
            }
            if (!found.has_value() || !target.IsValid())
                return Return::Failure(MakeError(TerrainEditErrors::Invalid));
            return Return::Success(*found);
        }

        Result<void> AddTarget(RawWeights &desired, const TerrainPaintTarget &target, const Binding &binding, std::uint8_t count) {
            if (!std::isfinite(target.weight) || target.weight <= 0 || target.weight > 1)
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            auto index = LayerIndex(binding, count, target.layer);
            if (index.HasError())
                return Result<void>::Failure(std::move(index).ErrorValue());
            const auto quantized = static_cast<std::uint32_t>(std::llround(double(target.weight) * 65535));
            if (desired[index.Value()] != 0 || quantized == 0)
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            desired[index.Value()] = quantized;
            return Result<void>::Success();
        }

        Result<Weights> DesiredWeights(const Binding &binding, std::uint8_t count, const TerrainPaintBrush &brush) {
            using Return = Result<Weights>;
            if (brush.targetCount == 0)
                return Return::Failure(MakeError(TerrainEditErrors::Invalid));
            RawWeights desired{};
            for (std::size_t i = 0; i < brush.targetCount; ++i) {
                auto added = AddTarget(desired, brush.targets[i], binding, count);
                if (added.HasError())
                    return Return::Failure(std::move(added).ErrorValue());
            }
            auto normalized = Terrain::NormalizeTerrainMaterialWeights(std::span(desired).first(count));
            if (normalized.HasError())
                return Return::Failure(std::move(normalized).ErrorValue());
            return Return::Success(normalized.Value().values);
        }

        Result<PaintPlan> PreparePlan(const Terrain::TerrainCanonicalSource &source, const std::optional<Binding> &binding,
                                      const TerrainPaintBrush &brush, std::span<const TerrainSculptStamp> stamps,
                                      const TerrainSculptLimits &limits) {
            using Return = Result<PaintPlan>;
            const bool weights = brush.mode == TerrainPaintMode::Weights;
            const auto count = std::uint64_t{source.layerCount};
            const auto setup = weights ? 2 * count : 2;
            // Largest-remainder normalization makes at most N remainder passes, each scanning N channels.
            const auto perStamp = weights ? count * (count + 6) + 64 : 2;
            const auto fixed = weights ? count * (count + 8) + 64 : 0;
            auto footprint = PrepareTerrainBrushFootprint(source, brush.radiusMeters, stamps, limits, setup, perStamp, fixed);
            if (footprint.HasError())
                return Return::Failure(std::move(footprint).ErrorValue());
            PaintPlan result{footprint.Value()};
            if (weights) {
                auto desired = DesiredWeights(*binding, source.layerCount, brush);
                if (desired.HasError())
                    return Return::Failure(std::move(desired).ErrorValue());
                result.desired = desired.Value();
            }
            return Return::Success(result);
        }

        bool Normalized(std::span<const std::uint16_t> values) noexcept {
            return std::accumulate(values.begin(), values.end(), std::uint32_t{0}) == 65535;
        }

        /** @brief Copy only the affected rows and validate the exact original channel samples before staging a stroke. */
        Result<void> CopyWeightRows(TerrainRasterPatch &patch, const Terrain::TerrainCanonicalSource &source,
                                    const CancellationToken &token) {
            const auto rowCount = std::size_t{patch.rect.width} * source.layerCount;
            patch.weights.resize(rowCount * patch.rect.height);
            for (std::size_t z = 0; z < patch.rect.height; ++z) {
                if (token.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
                const auto start = ((z + patch.rect.z) * source.width + patch.rect.x) * source.layerCount;
                auto row = std::span(patch.weights).subspan(z * rowCount, rowCount);
                std::copy_n(source.weights.begin() + static_cast<std::ptrdiff_t>(start), rowCount, row.begin());
                for (std::size_t x = 0; x < patch.rect.width; ++x)
                    if (!Normalized(row.subspan(x * source.layerCount, source.layerCount)))
                        return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            }
            return Result<void>::Success();
        }

        Result<void> CopyHoleRows(TerrainRasterPatch &patch, const Terrain::TerrainCanonicalSource &source,
                                  const CancellationToken &token) {
            patch.holes.resize(std::size_t{patch.rect.width} * patch.rect.height);
            for (std::size_t z = 0; z < patch.rect.height; ++z) {
                if (token.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
                const auto start = (z + patch.rect.z) * source.width + patch.rect.x;
                auto row = std::span(patch.holes).subspan(z * patch.rect.width, patch.rect.width);
                std::copy_n(source.holes.begin() + static_cast<std::ptrdiff_t>(start), row.size(), row.begin());
                if (!std::ranges::all_of(row, [](auto hole) {
                    return hole <= 1;
                }))
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            }
            return Result<void>::Success();
        }

        /** @brief Exact integer mixture preserves the total before canonical largest-remainder rounding. */
        Result<void> Blend(std::span<std::uint16_t> pixel, const Weights &desired, std::uint32_t alpha) {
            RawWeights mixed{};
            for (std::size_t i = 0; i < pixel.size(); ++i)
                mixed[i] = std::uint32_t{pixel[i]} * (65535 - alpha) + std::uint32_t{desired[i]} * alpha;
            auto normalized = Terrain::NormalizeTerrainMaterialWeights(std::span(mixed).first(pixel.size()));
            if (normalized.HasError())
                return Result<void>::Failure(std::move(normalized).ErrorValue());
            std::copy_n(normalized.Value().values.begin(), pixel.size(), pixel.begin());
            return Result<void>::Success();
        }

        Result<void> ApplyPixel(TerrainRasterPatch &patch, const Terrain::TerrainCanonicalSource &source, const TerrainPaintBrush &brush,
                                const Weights &desired, const TerrainSculptStamp &stamp, std::uint32_t x, std::uint32_t z) {
            const auto sampleX = patch.rect.x + x;
            const auto sampleZ = patch.rect.z + z;
            const double distance = std::hypot((sampleX - stamp.sampleX) * source.coordinates.spacingX,
                                               (sampleZ - stamp.sampleZ) * source.coordinates.spacingZ);
            if (distance >= brush.radiusMeters)
                return Result<void>::Success();
            const auto index = std::size_t{z} * patch.rect.width + x;
            if (brush.mode != TerrainPaintMode::Weights) {
                patch.holes[index] = brush.mode == TerrainPaintMode::CarveHole ? 1 : 0;
                return Result<void>::Success();
            }
            const double edge = 1 - distance / brush.radiusMeters;
            const auto alpha = static_cast<std::uint32_t>(std::llround(double(brush.strength) * edge * edge * 65535));
            return Blend(std::span(patch.weights).subspan(index * source.layerCount, source.layerCount), desired, alpha);
        }

        Result<void> ApplyStamp(TerrainRasterPatch &patch, const Terrain::TerrainCanonicalSource &source, const TerrainPaintBrush &brush,
                                const Weights &desired, const TerrainSculptStamp &stamp, const CancellationToken &token) {
            for (std::uint32_t z = 0; z < patch.rect.height; ++z) {
                if (token.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
                for (std::uint32_t x = 0; x < patch.rect.width; ++x) {
                    auto applied = ApplyPixel(patch, source, brush, desired, stamp, x, z);
                    if (applied.HasError())
                        return applied;
                }
            }
            return Result<void>::Success();
        }

        Result<TerrainRasterPatch> BuildPatch(const Terrain::TerrainCanonicalSource &source, const TerrainPaintBrush &brush,
                                              const PaintPlan &plan, std::span<const TerrainSculptStamp> stamps,
                                              const CancellationToken &token) {
            using Return = Result<TerrainRasterPatch>;
            TerrainRasterPatch patch{.rect = plan.footprint.rect};
            if (auto copied =
                    brush.mode == TerrainPaintMode::Weights ? CopyWeightRows(patch, source, token) : CopyHoleRows(patch, source, token);
                copied.HasError())
                return Return::Failure(std::move(copied).ErrorValue());
            for (const auto &stamp : stamps) {
                auto applied = ApplyStamp(patch, source, brush, plan.desired, stamp, token);
                if (applied.HasError())
                    return Return::Failure(std::move(applied).ErrorValue());
            }
            return Return::Success(std::move(patch));
        }
    }  // namespace

    /** @copydoc ProduceTerrainPaint */
    Result<TerrainEditOperation> ProduceTerrainPaint(const Terrain::TerrainCanonicalSource &source,
                                                     const std::optional<Terrain::TerrainAuthoringLayerBinding> &binding,
                                                     const TerrainPaintFence &fence, TerrainEditOperationId operation,
                                                     const TerrainPaintBrush &brush, std::span<const TerrainSculptStamp> stamps,
                                                     const TerrainSculptLimits &limits, const CancellationToken &token) {
        using Return = Result<TerrainEditOperation>;
        if (token.IsCancellationRequested())
            return Return::Failure(MakeError(TerrainEditErrors::Cancelled));
        if (!SameDocument(source, fence.document) || !SameLayers(binding, fence))
            return Return::Failure(MakeError(TerrainEditErrors::StaleRevision));
        if (!operation.IsValid() || !ValidBrush(brush))
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        if (auto admitted = AdmitChannels(source, binding, brush.mode); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        try {
            auto plan = PreparePlan(source, binding, brush, stamps, limits);
            if (plan.HasError())
                return Return::Failure(std::move(plan).ErrorValue());
            auto patch = BuildPatch(source, brush, plan.Value(), stamps, token);
            if (patch.HasError())
                return Return::Failure(std::move(patch).ErrorValue());
            TerrainEditOperation result{.fence = fence.document, .operation = operation};
            result.patches.push_back(std::move(patch).Value());
            return Return::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        }
    }
}  // namespace Horo::Editor
