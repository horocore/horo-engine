#pragma once

#include "Horo/PCG/PCGPointGeneration.h"
#include "Horo/PCG/PCGSpatialSnapshot.h"

#include <cstddef>
#include <span>
#include <variant>

namespace Horo::PCG {
    struct PCGPointOutputBound;
    struct PCGCookedNode;
}  // namespace Horo::PCG

namespace Horo::PCG::detail {
    struct NodeExecutionContext;

    using PointGenerationSettings =
        std::variant<PCGGridGenerationSettings, PCGJitterGenerationSettings, PCGStratifiedGenerationSettings, PCGPoissonGenerationSettings>;

    /** @brief Borrowed immutable geometry resolved from the exact captured snapshot. */
    struct PointGenerationDomain final {
        const PCGGrid *grid{};
        const PCGBoxVolume *box{};
        std::size_t count{};
    };

    /** @brief Decodes the one catalog-owned generation schema without allocating or executing. */
    [[nodiscard]] Result<PointGenerationSettings> DecodePointGeneration(const PCGCookedNode &node);

    /** @brief Resolves exact source geometry and point count without allocating. */
    [[nodiscard]] Result<PointGenerationDomain> ResolvePointGeneration(const PointGenerationSettings &settings,
                                                                       const PCGSpatialSnapshot &spatial);

    /** @brief Exact finite kernel work and temporary storage, proved before workspace allocation. */
    struct PointGenerationCharge final {
        std::size_t scratchBytes{};
        std::size_t pointVisits{};
        std::size_t snapshotElementVisits{};
    };

    /** @brief Decodes only the exact canonical schema-1 jitter payload. */
    [[nodiscard]] Result<PCGJitterGenerationSettings> DecodeJitterSettings(std::span<const std::uint8_t> payload);
    /** @brief Decodes only the exact canonical schema-1 stratified payload. */
    [[nodiscard]] Result<PCGStratifiedGenerationSettings> DecodeStratifiedSettings(std::span<const std::uint8_t> payload);
    /** @brief Decodes only the exact canonical schema-1 Poisson-style payload. */
    [[nodiscard]] Result<PCGPoissonGenerationSettings> DecodePoissonSettings(std::span<const std::uint8_t> payload);

    /** @brief Proves complete attempts, pair checks, writes, source search and accepted-position scratch.
     * @param settings Validated exact node settings.
     * @param maximumPoints Admitted output ceiling; exact declared counts must fit it.
     * @param spatialElements Complete bounded source-search ceiling.
     * @return Checked charge or typed invalid/capacity/overflow failure; never allocates output or scratch. */
    [[nodiscard]] Result<PointGenerationCharge> ChargePointGeneration(const PointGenerationSettings &settings, std::size_t maximumPoints,
                                                                      std::size_t spatialElements);

    /** @brief Validates geometry and the exact typed identity column before any kernel writes.
     * @return Success or explicit missing-source, density, budget, schema or finite-geometry failure. */
    [[nodiscard]] Result<void> ValidatePointGeneration(const PointGenerationSettings &settings, const PCGSpatialSnapshot &spatial,
                                                       const PCGPointOutputBound &output);

    /** @brief Writes one admitted generation output in semantic sample order.
     * @pre Complete kernel charges and source/schema feasibility passed before workspace creation.
     * @return Sealed complete output or typed failure; the evaluator cancels all partial workspace state on failure. */
    [[nodiscard]] Result<void> ExecutePointGeneration(const NodeExecutionContext &context, std::uint32_t node,
                                                      const PointGenerationSettings &settings);
}  // namespace Horo::PCG::detail
