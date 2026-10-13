#pragma once
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/PCG/PCGNodeCatalog.h"
#include "Horo/PCG/PCGPointCloudWorkspace.h"
#include "Horo/PCG/PCGPointFiltering.h"
#include "Horo/PCG/PCGSpatialSnapshot.h"

namespace Horo::PCG::detail {
    /** @brief Identifies the closed settings-backed filtering extension. */
    [[nodiscard]] bool IsPointFilter(PCGCpuNodeKind kind) noexcept;
    /** @brief Decodes an exact canonical payload without unbounded allocation. */
    [[nodiscard]] Result<PCGPointFilterSettings> DecodePointFilter(const PCGCookedNode &node);
    /** @brief Proves complete mask/pair/surface work before workspace allocation. */
    [[nodiscard]] Result<PCGNodeCost> ChargePointFilter(PCGCpuNodeKind kind, std::size_t points, std::size_t spatialElements);
    /** @brief Validates exact schema and spatial prerequisites before any output allocation. */
    [[nodiscard]] Result<void> ValidatePointFilter(const PCGPointFilterSettings &settings, const PCGPointSchema &schema,
                                                   const PCGSpatialSnapshot &spatial);
    /** @brief Selects stable source indices with bounded scratch and cancellation; never mutates input. */
    [[nodiscard]] Result<std::vector<std::size_t>> SelectPointFilter(const PCGPointFilterSettings &settings, const PCGPointReadView &source,
                                                                     const PCGSpatialSnapshot &spatial, CancellationToken cancellation);
}  // namespace Horo::PCG::detail
