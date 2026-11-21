#pragma once

/**
 * @file EnvironmentQueryPerception.h
 * @brief Authorized perception facts for a generation-fenced tactical query test.
 */

#include "Horo/AI/EnvironmentQueryExecution.h"

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

    /** @brief Inert native actor, perceived-entity generator and visibility test descriptors. */
    struct BuiltinPerceptionQueryDescriptors final {
        QueryItemTypeDescriptor item;
        QueryGeneratorDescriptor generator;
        QueryTestDescriptor test;
    };

    /** @brief Returns the native Perception descriptors without reading any observation. */
    [[nodiscard]] BuiltinPerceptionQueryDescriptors DescribeBuiltinPerceptionQuery();

    /** @brief Creates a native query that ranks remembered or currently sensed actors by confidence.
     * @param query Authored query ID. @param generator Candidate-generation stage ID.
     * @param test Confidence/visibility stage ID. @param result Authored result schema.
     * @param displayName Authored label. @param match Which observations are eligible.
     * @param maximumResults Cap on published ranked actors.
     * @return Detached authoring asset or a typed schema failure.
     */
    [[nodiscard]] Result<EnvironmentQueryAsset> CreateBuiltinPerceptionQuery(QueryId query, QueryStageId generator, QueryStageId test,
                                                                             QueryResultSchemaId result, std::string displayName,
                                                                             QueryPerceptionMatch match, std::uint32_t maximumResults);

    /** @brief Identifies a plan selecting the native perceived-entity generator. */
    [[nodiscard]] bool UsesBuiltinPerceptionQuery(const EnvironmentQueryPlan &plan) noexcept;

    /** @brief Binds a compiled perception query to the exact Scene-authorized observation snapshot.
     * @param plan Cooked query. @param registry Matching inert descriptors.
     * @param inputs Query execution identity and context. @param perception Exact authorized caller snapshot.
     * @param settings Bounded execution policy and deadline.
     * @return Staged execution or a typed failure. Perception queries bypass shared result reuse.
     */
    [[nodiscard]] Result<std::unique_ptr<QueryExecution>> AdmitBuiltinPerceptionQuery(const EnvironmentQueryPlan &plan,
                                                                                      const QuerySchemaRegistry &registry,
                                                                                      QueryExecutionInputs inputs,
                                                                                      PerceptionQueryInput perception,
                                                                                      QueryExecutionSettings settings);
}  // namespace Horo::AI
