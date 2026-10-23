#include "Horo/Editor/TerrainBrushTools.h"

#include "Horo/Terrain/TerrainMaterial.h"
#include "TerrainBrushAdmission.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Translates planner admission without changing document state. */
        Result<TerrainEditOperation> Failed(const ErrorCodeDescriptor &error) {
            return Result<TerrainEditOperation>::Failure(MakeError(error));
        }

        /** @brief Keeps supported planner modes separate from scalar request admission. */
        bool SupportedBrushMode(TerrainBrushMode mode) {
            return mode == TerrainBrushMode::Raise || mode == TerrainBrushMode::Lower || mode == TerrainBrushMode::Paint ||
                   mode == TerrainBrushMode::Carve;
        }

        /** @brief Validates gesture identity and settings in their original evaluation order. */
        bool InvalidBrushRequest(const TerrainBrushSettings &settings, std::span<const TerrainBrushDab> dabs,
                                 TerrainEditOperationId operation) {
            return !operation.IsValid() || dabs.empty() || !std::isfinite(settings.radiusMeters) || settings.radiusMeters <= 0 ||
                   !std::isfinite(settings.strength) || settings.strength < 0 || !SupportedBrushMode(settings.mode);
        }

        /** @brief Admits the request before borrowing canonical source data. */
        const ErrorCodeDescriptor *AdmitBrushRequest(const TerrainAuthoringDocument &document, const TerrainBrushSettings &settings,
                                                     std::span<const TerrainBrushDab> dabs, TerrainEditOperationId operation,
                                                     const TerrainBrushLimits &limits, const CancellationToken &cancellation) {
            if (document.IsClosed())
                return &TerrainEditErrors::Closed;
            if (cancellation.IsCancellationRequested())
                return &TerrainEditErrors::Cancelled;
            if (InvalidBrushRequest(settings, dabs, operation))
                return &TerrainEditErrors::Invalid;
            if (dabs.size() > limits.maximumDabs)
                return &TerrainEditErrors::LimitExceeded;
            return nullptr;
        }

        struct BrushGeometry final {
            TerrainPatchRect rect{};
            std::uint64_t count{};
        };

        /** @brief Rejects nonfinite and out-of-source dab coordinates without narrowing. */
        bool InvalidBrushDab(const Terrain::TerrainCanonicalSource &source, const TerrainBrushDab &dab) {
            return !std::isfinite(dab.x) || !std::isfinite(dab.z) || dab.x < 0 || dab.z < 0 || dab.x > source.width - 1.0 ||
                   dab.z > source.height - 1.0;
        }

        /** @brief Requires existing authored channels and legal categorical/blend strength without initialization. */
        bool InvalidBrushSource(const Terrain::TerrainCanonicalSource &source, const TerrainBrushSettings &settings) {
            if (settings.mode == TerrainBrushMode::Carve)
                return settings.strength > 1.0F || !source.HasHoles();
            return settings.mode == TerrainBrushMode::Paint &&
                   (settings.strength > 1.0F || source.layerCount == 0 || settings.layer >= source.layerCount);
        }

        /** @brief Charges all interleaved weight reads and owned bytes before detached allocation. */
        bool CanOwnBrushPatch(const Terrain::TerrainCanonicalSource &source, const TerrainBrushSettings &settings, std::uint64_t count,
                              std::size_t dabs, const TerrainBrushLimits &limits) {
            if (settings.mode == TerrainBrushMode::Carve)
                return limits.maximumOwnedBytes >= sizeof(TerrainRasterPatch) &&
                       count <= (limits.maximumOwnedBytes - sizeof(TerrainRasterPatch)) / sizeof(std::uint8_t) &&
                       dabs < limits.maximumReadVisits && count <= limits.maximumReadVisits / (dabs + 1);
            if (settings.mode != TerrainBrushMode::Paint)
                return Detail::CanOwnTerrainHeightPatch(count, limits.maximumOwnedBytes);
            const std::size_t stride = source.layerCount * sizeof(std::uint16_t);
            if (limits.maximumOwnedBytes < sizeof(TerrainRasterPatch))
                return false;
            return count <= (limits.maximumOwnedBytes - sizeof(TerrainRasterPatch)) / stride &&
                   count <= limits.maximumReadVisits / source.layerCount / (dabs + 1);
        }

        /** @brief Owns ordered stroke bounds, rectangle conversion and patch resource admission. */
        const ErrorCodeDescriptor *AdmitBrushGeometry(const Terrain::TerrainCanonicalSource &source, const TerrainBrushSettings &settings,
                                                      std::span<const TerrainBrushDab> dabs, const TerrainBrushLimits &limits,
                                                      BrushGeometry &geometry) {
            const double rx = settings.radiusMeters / source.coordinates.spacingX;
            const double rz = settings.radiusMeters / source.coordinates.spacingZ;
            double x0 = source.width - 1.0, z0 = source.height - 1.0, x1 = 0, z1 = 0;
            for (const auto &dab : dabs) {
                if (InvalidBrushDab(source, dab))
                    return &TerrainEditErrors::Invalid;
                x0 = std::min(x0, std::max(0.0, std::ceil(dab.x - rx)));
                z0 = std::min(z0, std::max(0.0, std::ceil(dab.z - rz)));
                x1 = std::max(x1, std::min(source.width - 1.0, std::floor(dab.x + rx)));
                z1 = std::max(z1, std::min(source.height - 1.0, std::floor(dab.z + rz)));
            }
            if (x0 > x1 || z0 > z1)
                return &TerrainEditErrors::Invalid;
            const TerrainPatchRect rect{static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(z0),
                                        static_cast<std::uint32_t>(x1 - x0 + 1), static_cast<std::uint32_t>(z1 - z0 + 1)};
            const std::uint64_t count = static_cast<std::uint64_t>(rect.width) * rect.height;
            if (count > limits.maximumSamples || !CanOwnBrushPatch(source, settings, count, dabs.size(), limits) ||
                count > limits.maximumReadVisits / (dabs.size() + 1))
                return &TerrainEditErrors::LimitExceeded;
            geometry = {.rect = rect, .count = count};
            return nullptr;
        }

        enum class HeightPatchStatus {
            Ready,
            Cancelled,
            Invalid
        };

        /** @brief Fills and applies ordered height dabs to a detached patch using borrowed inputs. */
        HeightPatchStatus PrepareHeightPatch(const Terrain::TerrainCanonicalSource &source, const TerrainBrushSettings &settings,
                                             std::span<const TerrainBrushDab> dabs, const CancellationToken &cancellation,
                                             std::uint64_t count, TerrainRasterPatch &patch) {
            const auto &rect = patch.rect;
            patch.heightsMeters.reserve(static_cast<std::size_t>(count));
            for (std::uint32_t z = rect.z; z < rect.z + rect.height; ++z)
                for (std::uint32_t x = rect.x; x < rect.x + rect.width; ++x)
                    patch.heightsMeters.push_back(source.heightsMeters[static_cast<std::size_t>(z) * source.width + x]);
            for (const auto &dab : dabs) {
                if (cancellation.IsCancellationRequested())
                    return HeightPatchStatus::Cancelled;
                for (std::uint32_t z = 0; z < rect.height; ++z)
                    for (std::uint32_t x = 0; x < rect.width; ++x) {
                        const double dx = (rect.x + x - dab.x) * source.coordinates.spacingX;
                        const double dz = (rect.z + z - dab.z) * source.coordinates.spacingZ;
                        const double falloff = std::max(0.0, 1.0 - std::hypot(dx, dz) / settings.radiusMeters);
                        float &height = patch.heightsMeters[static_cast<std::size_t>(z) * rect.width + x];
                        const double changed =
                            height + (settings.mode == TerrainBrushMode::Lower ? -settings.strength : settings.strength) * falloff;
                        if (!Detail::TryNarrowTerrainHeight(changed, height))
                            return HeightPatchStatus::Invalid;
                    }
            }
            return HeightPatchStatus::Ready;
        }

        /** @brief Blends one admitted canonical sample using the shared authored-order normalizer. */
        Result<void> BlendPaintSample(const TerrainBrushSettings &settings, double falloff, std::span<std::uint16_t> weights) {
            const double alpha = std::clamp(static_cast<double>(settings.strength) * falloff, 0.0, 1.0);
            const auto amount = static_cast<std::uint32_t>(std::floor(alpha * 65'535.0 + 0.5));
            std::array<std::uint32_t, Terrain::TerrainDescriptorHardLimits::LayersPerTile> numerators{};
            // Canonical weights sum to 65535: each numerator is at most 65535 squared, within uint32.
            for (std::size_t layer = 0; layer < weights.size(); ++layer)
                numerators[layer] =
                    (65'535U - amount) * static_cast<std::uint32_t>(weights[layer]) + (layer == settings.layer ? amount * 65'535U : 0U);
            auto normalized = Terrain::NormalizeTerrainMaterialWeights(std::span<const std::uint32_t>{numerators.data(), weights.size()});
            if (normalized.HasError())
                return Result<void>::Failure(std::move(normalized).ErrorValue());
            std::copy_n(normalized.Value().values.begin(), weights.size(), weights.begin());
            return Result<void>::Success();
        }

        /** @brief Applies one ordered dab only to the planner-owned interleaved weight buffer. */
        Result<void> ApplyPaintDab(const Terrain::TerrainCanonicalSource &source, const TerrainBrushSettings &settings,
                                   const TerrainBrushDab &dab, TerrainRasterPatch &patch) {
            const auto &rect = patch.rect;
            for (std::uint32_t z = 0; z < rect.height; ++z)
                for (std::uint32_t x = 0; x < rect.width; ++x) {
                    const double dx = (rect.x + x - dab.x) * source.coordinates.spacingX;
                    const double dz = (rect.z + z - dab.z) * source.coordinates.spacingZ;
                    const double falloff = std::max(0.0, 1.0 - std::hypot(dx, dz) / settings.radiusMeters);
                    if (!std::isfinite(falloff))
                        return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
                    const auto offset = (static_cast<std::size_t>(z) * rect.width + x) * source.layerCount;
                    auto blended =
                        BlendPaintSample(settings, falloff, std::span<std::uint16_t>{patch.weights.data() + offset, source.layerCount});
                    if (blended.HasError())
                        return Result<void>::Failure(std::move(blended).ErrorValue());
                }
            return Result<void>::Success();
        }

        /** @brief Copies the bounded canonical rectangle and normalizes after each caller-ordered dab. */
        Result<void> PreparePaintPatch(const Terrain::TerrainCanonicalSource &source, const TerrainBrushSettings &settings,
                                       std::span<const TerrainBrushDab> dabs, const CancellationToken &cancellation, std::uint64_t count,
                                       TerrainRasterPatch &patch) {
            const auto &rect = patch.rect;
            patch.weights.reserve(static_cast<std::size_t>(count) * source.layerCount);
            for (std::uint32_t z = rect.z; z < rect.z + rect.height; ++z) {
                const auto offset = (static_cast<std::size_t>(z) * source.width + rect.x) * source.layerCount;
                const auto begin = source.weights.begin() + static_cast<std::ptrdiff_t>(offset);
                patch.weights.insert(patch.weights.end(), begin, begin + static_cast<std::ptrdiff_t>(rect.width * source.layerCount));
            }
            for (const auto &dab : dabs) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
                auto applied = ApplyPaintDab(source, settings, dab, patch);
                if (applied.HasError())
                    return Result<void>::Failure(std::move(applied).ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Applies the categorical half-influence threshold to detached per-vertex hole bytes only. */
        Result<void> ApplyCarveDab(const Terrain::TerrainCanonicalSource &source, const TerrainBrushSettings &settings,
                                   const TerrainBrushDab &dab, const CancellationToken &cancellation, TerrainRasterPatch &patch) {
            const auto &rect = patch.rect;
            for (std::uint32_t z = 0; z < rect.height; ++z) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
                for (std::uint32_t x = 0; x < rect.width; ++x) {
                    const double dx = (rect.x + x - dab.x) * source.coordinates.spacingX;
                    const double dz = (rect.z + z - dab.z) * source.coordinates.spacingZ;
                    const double falloff = std::max(0.0, 1.0 - std::hypot(dx, dz) / settings.radiusMeters);
                    const double influence = static_cast<double>(settings.strength) * falloff;
                    if (!std::isfinite(influence))
                        return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
                    if (std::clamp(influence, 0.0, 1.0) >= 0.5)
                        patch.holes[static_cast<std::size_t>(z) * rect.width + x] = 1;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Snapshots the bounded authored mask and applies dabs in caller order without touching the owner. */
        Result<void> PrepareCarvePatch(const Terrain::TerrainCanonicalSource &source, const TerrainBrushSettings &settings,
                                       std::span<const TerrainBrushDab> dabs, const CancellationToken &cancellation, std::uint64_t count,
                                       TerrainRasterPatch &patch) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
            const auto &rect = patch.rect;
            patch.holes.reserve(static_cast<std::size_t>(count));
            for (std::uint32_t z = rect.z; z < rect.z + rect.height; ++z) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
                for (std::uint32_t x = rect.x; x < rect.x + rect.width; ++x)
                    patch.holes.push_back(source.holes[static_cast<std::size_t>(z) * source.width + x]);
            }
            for (const auto &dab : dabs) {
                auto applied = ApplyCarveDab(source, settings, dab, cancellation, patch);
                if (applied.HasError())
                    return Result<void>::Failure(std::move(applied).ErrorValue());
            }
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
            return Result<void>::Success();
        }

        /** @brief Routes admitted channel work and preserves typed cancellation before operation assembly. */
        Result<void> PrepareBrushPatch(const Terrain::TerrainCanonicalSource &source, const TerrainBrushSettings &settings,
                                       std::span<const TerrainBrushDab> dabs, const CancellationToken &cancellation, std::uint64_t count,
                                       TerrainRasterPatch &patch) {
            if (settings.mode == TerrainBrushMode::Paint)
                return PreparePaintPatch(source, settings, dabs, cancellation, count, patch);
            if (settings.mode == TerrainBrushMode::Carve)
                return PrepareCarvePatch(source, settings, dabs, cancellation, count, patch);
            const auto status = PrepareHeightPatch(source, settings, dabs, cancellation, count, patch);
            if (status == HeightPatchStatus::Cancelled)
                return Result<void>::Failure(MakeError(TerrainEditErrors::Cancelled));
            if (status == HeightPatchStatus::Invalid)
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PlanTerrainBrush */
    Result<TerrainEditOperation> PlanTerrainBrush(const TerrainAuthoringDocument &document, const TerrainBrushSettings &settings,
                                                  std::span<const TerrainBrushDab> dabs, TerrainEditOperationId operation,
                                                  const TerrainBrushLimits &limits, const CancellationToken &cancellation) {
        if (const auto *failure = AdmitBrushRequest(document, settings, dabs, operation, limits, cancellation))
            return Failed(*failure);
        const auto &source = document.Source();
        if (InvalidBrushSource(source, settings))
            return Failed(TerrainEditErrors::Invalid);
        BrushGeometry geometry;
        if (const auto *failure = AdmitBrushGeometry(source, settings, dabs, limits, geometry))
            return Failed(*failure);
        const auto &rect = geometry.rect;
        const auto count = geometry.count;
        try {
            TerrainRasterPatch patch{.rect = rect};
            auto prepared = PrepareBrushPatch(source, settings, dabs, cancellation, count, patch);
            if (prepared.HasError())
                return Result<TerrainEditOperation>::Failure(std::move(prepared).ErrorValue());
            TerrainEditOperation result{.fence = document.Fence(), .operation = operation};
            result.patches.push_back(std::move(patch));
            return Result<TerrainEditOperation>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Failed(TerrainEditErrors::LimitExceeded);
        }
    }
}  // namespace Horo::Editor
