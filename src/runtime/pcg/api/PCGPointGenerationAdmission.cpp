#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGPointCloudWorkspace.h"
#include "PCGPointGenerationInternal.h"
#include "PCGPointIdentityInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <type_traits>

namespace Horo::PCG::detail {
    namespace {
        /** @brief Preserves the owning typed error domain at the kernel boundary. */
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Proves a three-axis count without wrapping size_t. */
        Result<std::size_t> CellCount(const std::array<std::uint32_t, 3> &dimensions) {
            const auto xy = CheckedPCGMultiply(dimensions[0], dimensions[1]);
            if (xy.HasError())
                return xy;
            return CheckedPCGMultiply(xy.Value(), dimensions[2]);
        }

        /** @brief Resolves a grid by provider-issued identity, never by container position. */
        Result<PointGenerationDomain> ResolveGrid(const SpatialElementId id, const PCGSpatialSnapshot &spatial,
                                                  const ErrorCodeDescriptor &missing) {
            const auto found = std::ranges::find(spatial.Grids(), id, &PCGGrid::id);
            if (found == spatial.Grids().end())
                return Reject<PointGenerationDomain>(missing);
            const auto count = CellCount(found->dimensions);
            if (count.HasError())
                return Result<PointGenerationDomain>::Failure(count.ErrorValue());
            return Result<PointGenerationDomain>::Success({&*found, nullptr, count.Value()});
        }

        /** @brief Rejects unsupported geometry rather than approximating a sphere by its bounding box. */
        Result<const PCGBoxVolume *> ResolveBox(const SpatialElementId id, const PCGSpatialSnapshot &spatial) {
            for (const auto &volume : spatial.Volumes()) {
                const bool matched = std::visit([id]<typename T>(const T &value) {
                    return value.id == id;
                }, volume);
                if (!matched)
                    continue;
                if (const auto *box = std::get_if<PCGBoxVolume>(&volume))
                    return Result<const PCGBoxVolume *>::Success(box);
                return Reject<const PCGBoxVolume *>(PCGErrors::CpuEvaluationUnsupported);
            }
            return Reject<const PCGBoxVolume *>(PCGErrors::SpatialInputInvalid);
        }

        /** @brief Each half-distance cell has diameter below the separation; at most one point can inhabit it.
         * Saturating at requested count avoids overflow for tiny distances and extremely large finite domains. */
        bool DensityCanFit(const PCGBoxVolume &box, const PCGPoissonGenerationSettings &settings) {
            if (settings.count < 2)
                return true;
            const std::array<double, 3> extent{static_cast<double>(box.bounds.maximum.x) - box.bounds.minimum.x,
                                               static_cast<double>(box.bounds.maximum.y) - box.bounds.minimum.y,
                                               static_cast<double>(box.bounds.maximum.z) - box.bounds.minimum.z};
            std::size_t cells = 1;
            for (const double axis : extent) {
                // Division may become infinite for subnormal separation: that implies an upper bound >= count.
                const double count = std::ceil((axis / settings.minimumDistance) * 2.0);
                if (!std::isfinite(count) || count >= static_cast<double>(settings.count))
                    return true;
                const auto axisCells = static_cast<std::size_t>(std::max(1.0, count));
                if (cells >= (static_cast<std::size_t>(settings.count) - 1) / axisCells + 1)
                    return true;
                // The failed saturation comparison proves the next product is <= count-1.
                cells *= axisCells;
            }
            return cells >= settings.count;
        }

        /** @brief New generators require a typed semantic identity; Grid v1 permits its optional declaration. */
        Result<void> ValidateIdentity(const PointGenerationSettings &settings, const PCGPointOutputBound &output) {
            if (!output.schema)
                return Reject<void>(PCGErrors::PointSchemaInvalid);
            const auto identity = HasPointIdentity(*output.schema);
            if (identity.HasError())
                return Result<void>::Failure(identity.ErrorValue());
            if (identity.Value() || std::holds_alternative<PCGGridGenerationSettings>(settings))
                return Result<void>::Success();
            return Reject<void>(PCGErrors::GenerationIdentityInvalid);
        }

        /** @brief Charges bounded attempts, all possible accepted-prefix comparisons and temporary positions. */
        Result<PointGenerationCharge> PoissonCharge(const PCGPoissonGenerationSettings &settings, const std::size_t maximumPoints,
                                                    const std::size_t spatialElements) {
            if (settings.count > maximumPoints)
                return Reject<PointGenerationCharge>(PCGErrors::CpuEvaluationCapacityExceeded);
            if (settings.attempts < settings.count)
                return Reject<PointGenerationCharge>(PCGErrors::GenerationAttemptsExhausted);
            const auto pairs = CheckedPCGMultiply(settings.attempts, settings.count);
            if (pairs.HasError())
                return Result<PointGenerationCharge>::Failure(pairs.ErrorValue());
            const auto visits = CheckedPCGAdd(pairs.Value(), settings.attempts);
            if (visits.HasError())
                return Result<PointGenerationCharge>::Failure(visits.ErrorValue());
            const auto writes = CheckedPCGAdd(visits.Value(), settings.count);
            if (writes.HasError())
                return Result<PointGenerationCharge>::Failure(writes.ErrorValue());
            const auto scratch = CheckedPCGMultiply(settings.count, sizeof(Math::Vec3));
            if (scratch.HasError())
                return Result<PointGenerationCharge>::Failure(scratch.ErrorValue());
            return Result<PointGenerationCharge>::Success({scratch.Value(), writes.Value(), spatialElements});
        }
    }  // namespace

    /** @copydoc ResolvePointGeneration */
    Result<PointGenerationDomain> ResolvePointGeneration(const PointGenerationSettings &settings, const PCGSpatialSnapshot &spatial) {
        return std::visit([&spatial]<typename T>(const T &value) -> Result<PointGenerationDomain> {
            if constexpr (std::is_same_v<T, PCGGridGenerationSettings> || std::is_same_v<T, PCGJitterGenerationSettings>) {
                return ResolveGrid(value.grid, spatial,
                                   std::is_same_v<T, PCGGridGenerationSettings> ? PCGErrors::CpuEvaluationInvalid
                                                                                : PCGErrors::SpatialInputInvalid);
            } else {
                const auto box = ResolveBox(value.volume, spatial);
                if (box.HasError())
                    return Result<PointGenerationDomain>::Failure(box.ErrorValue());
                if constexpr (std::is_same_v<T, PCGStratifiedGenerationSettings>) {
                    const auto count = CellCount(value.strata);
                    if (count.HasError())
                        return Result<PointGenerationDomain>::Failure(count.ErrorValue());
                    return Result<PointGenerationDomain>::Success({nullptr, box.Value(), count.Value()});
                } else {
                    return Result<PointGenerationDomain>::Success({nullptr, box.Value(), value.count});
                }
            }
        }, settings);
    }

    /** @copydoc ChargePointGeneration */
    Result<PointGenerationCharge> ChargePointGeneration(const PointGenerationSettings &settings, const std::size_t maximumPoints,
                                                        const std::size_t spatialElements) {
        // One admission geometry proof and one immutable execution lookup use the same bounded source vocabulary.
        const auto searches = CheckedPCGMultiply(spatialElements, 2);
        if (searches.HasError())
            return Result<PointGenerationCharge>::Failure(searches.ErrorValue());
        if (const auto *poisson = std::get_if<PCGPoissonGenerationSettings>(&settings))
            return PoissonCharge(*poisson, maximumPoints, searches.Value());
        std::size_t count = maximumPoints;
        if (const auto *stratified = std::get_if<PCGStratifiedGenerationSettings>(&settings)) {
            const auto exact = CellCount(stratified->strata);
            if (exact.HasError())
                return Result<PointGenerationCharge>::Failure(exact.ErrorValue());
            count = exact.Value();
        }
        if (count > maximumPoints)
            return Reject<PointGenerationCharge>(PCGErrors::CpuEvaluationCapacityExceeded);
        return Result<PointGenerationCharge>::Success({0, count, searches.Value()});
    }

    /** @copydoc ValidatePointGeneration */
    Result<void> ValidatePointGeneration(const PointGenerationSettings &settings, const PCGSpatialSnapshot &spatial,
                                         const PCGPointOutputBound &output) {
        if (const auto identity = ValidateIdentity(settings, output); identity.HasError())
            return identity;
        const auto domain = ResolvePointGeneration(settings, spatial);
        if (domain.HasError())
            return Result<void>::Failure(domain.ErrorValue());
        if (domain.Value().count > output.maximumPoints)
            return Reject<void>(PCGErrors::CpuEvaluationCapacityExceeded);
        if (const auto *poisson = std::get_if<PCGPoissonGenerationSettings>(&settings)) {
            if (poisson->attempts < poisson->count)
                return Reject<void>(PCGErrors::GenerationAttemptsExhausted);
            if (!DensityCanFit(*domain.Value().box, *poisson))
                return Reject<void>(PCGErrors::GenerationDensityImpossible);
        }
        return Result<void>::Success();
    }
}  // namespace Horo::PCG::detail
