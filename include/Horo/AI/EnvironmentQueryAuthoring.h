#pragma once
/** @file EnvironmentQueryAuthoring.h
 * @brief Typed input and host capability for initial point-lattice query authoring.
 */

#include "Horo/AI/EnvironmentQuerySchema.h"

#include <optional>

namespace Horo::AI {
    /** @brief Maximum bounded radius of the supported native point lattice in steps. */
    inline constexpr std::uint32_t MaximumQueryLatticeOuterSteps = 31;

    /** @brief Horizontal square or annulus sampled on an exact millimeter lattice. */
    struct QueryPointLattice final {
        std::uint32_t stepMillimeters{};
        std::uint32_t outerSteps{};
        std::optional<std::uint32_t> innerSteps; /**< Absent for a square; present for a closed annulus. */
    };

    /** @brief Filters lattice candidates by distance from the captured querier, with optional nearest-first scoring. */
    struct QueryPointDistanceTest final {
        std::uint32_t radiusMillimeters{};
        bool preferNearer{};
    };

    /** @brief Pure initial-source factory installed by the application with the matching provider registry. */
    using LatticeQueryAuthoringFactory = Result<EnvironmentQueryAsset> (*)(QueryId, QueryStageId, QueryResultSchemaId, std::string,
                                                                           const QueryPointLattice &, std::uint32_t);
}  // namespace Horo::AI
