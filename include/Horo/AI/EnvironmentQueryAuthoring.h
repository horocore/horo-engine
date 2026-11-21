#pragma once
/** @file EnvironmentQueryAuthoring.h
 * @brief Typed input and host capability for initial point-lattice query authoring.
 */

#include "Horo/AI/EnvironmentQuerySchema.h"

#include <array>
#include <optional>
#include <vector>

namespace Horo::AI {
    /** @brief Maximum bounded radius of the supported native point lattice in steps. */
    inline constexpr std::uint32_t MaximumQueryLatticeOuterSteps = 31;

    /** @brief Horizontal square or annulus sampled on an exact millimeter lattice. */
    struct QueryPointLattice final {
        std::uint32_t stepMillimeters{};
        std::uint32_t outerSteps{};
        std::optional<std::uint32_t> innerSteps; /**< Absent for a square; present for a closed annulus. */
        QueryContextId center;                   /**< An invalid identity selects the registered default. */
    };

    /** @brief Filters lattice candidates by distance from a captured location, with optional nearest-first scoring. */
    struct QueryPointDistanceTest final {
        std::uint32_t radiusMillimeters{};
        bool preferNearer{};
        std::uint32_t scoreWeight{1}; /**< Positive relative weight when nearest-first scoring is enabled. */
        QueryContextId center;        /**< An invalid identity selects the registered default. */
    };

    /** @brief Stable location choice and its optional provider-owned authoring label. */
    struct QueryPointContextOption final {
        QueryContextId id;
        std::string displayName;
    };

    /** @brief Existing distance stage and its editable controls, keyed by durable identity. */
    struct QueryPointDistanceStageSettings final {
        QueryStageId stage;
        QueryPointDistanceTest test;
        std::vector<QueryPointContextOption> centers; /**< Available authored choices in registered order. */
    };

    /** @brief Editable Physics stage using the canonical project collision channel identity. */
    struct QueryPointVisibilityStageSettings final {
        QueryStageId stage;
        std::array<std::uint8_t, 16> channel;
    };

    /** @brief Editable Navigation stage with an optional positive traversal cost ceiling for ranking. */
    struct QueryPointNavigationStageSettings final {
        QueryStageId stage;
        std::optional<double> maximumScoredCost;
    };

    /** @brief Editable settings of the admitted native lattice and its existing distance stages. */
    struct BuiltinPointQuerySettings final {
        QueryPointLattice lattice;
        std::uint32_t maximumResults{};
        std::vector<QueryPointDistanceStageSettings> distanceTests; /**< Admitted semantic execution order. */
        std::vector<QueryPointContextOption> latticeCenters;        /**< Available generator choices in registered order. */
        std::optional<QueryPointVisibilityStageSettings> visibility;
        std::optional<QueryPointNavigationStageSettings> navigation;
    };

    /** @brief Pure initial-source factory installed by the application with the matching provider registry. */
    using LatticeQueryAuthoringFactory = Result<EnvironmentQueryAsset> (*)(QueryId, QueryStageId, QueryResultSchemaId, std::string,
                                                                           const QueryPointLattice &, std::uint32_t);
    /** @brief Which captured Perception observations can become scored actors. */
    enum class QueryPerceptionMatch : std::uint8_t {
        Remembered,
        CurrentlySensed,
    };

    /** @brief Editable native Perception query policy. */
    struct BuiltinPerceptionQuerySettings final {
        QueryPerceptionMatch match{QueryPerceptionMatch::CurrentlySensed};
        std::uint32_t maximumResults{8};
    };
}  // namespace Horo::AI
