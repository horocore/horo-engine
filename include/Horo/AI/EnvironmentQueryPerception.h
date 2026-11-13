#pragma once

/**
 * @file EnvironmentQueryPerception.h
 * @brief Authorized perception facts for a generation-fenced tactical query test.
 */

#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQueryScoring.h"

namespace Horo::AI {
    /** @brief Captured policy for which observations may contribute to the test. */
    enum class QueryPerceptionMatch : std::uint8_t {
        Remembered,
        CurrentlySensed,
    };

    /** @brief Exact query stage, perception publication and filter. */
    struct QueryPerceptionTest final {
        QueryStageId stage;
        AgentHandle agent;
        std::uint64_t revision{};
        QueryPerceptionMatch match{QueryPerceptionMatch::Remembered};
        std::optional<SenseTypeId> sense;
    };

    /** @brief Projects current authorized memory into one test sample.
     * @param source Owner-lane scene and perception publication.
     * @param test Captured stage, listener owner, revision and match policy.
     * @param candidate Exact live candidate entity.
     * @return A matching filter and the highest eligible confidence, or a typed stale or invalid failure.
     */
    [[nodiscard]] Result<QueryTestSample> EvaluateQueryPerception(const QueryPerceivedEntitySource &source, const QueryPerceptionTest &test,
                                                                  Runtime::EntityRef candidate);
}  // namespace Horo::AI
