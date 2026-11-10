#pragma once

/**
 * @file EnvironmentQueryScoring.h
 * @brief Fixed-range scoring and deterministic best-N selection for admitted EQS test stages.
 */

#include "Horo/AI/EnvironmentQuerySchema.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::AI {
    /** @brief Response shape applied after mapping a finite metric into the declared range. */
    enum class QueryScoreCurve : std::uint8_t {
        Linear,
        SmoothStep,
        Threshold,
        Count
    };

    /** @brief Metric preference before applying the response curve. */
    enum class QueryScoreDirection : std::uint8_t {
        Higher,
        Lower,
        Count
    };

    /** @brief Optional secondary ordering after the total score. Stable candidate index always breaks the final tie. */
    enum class QueryScoreTieBreak : std::uint8_t {
        StableIndex,
        LowerKey,
        HigherKey,
        Count
    };

    /** @brief Immutable numerical policy for a test stage that contributes to the total score. */
    struct QueryScoreRange final {
        double minimum{};
        double maximum{1};
        double weight{1};
        QueryScoreCurve curve{QueryScoreCurve::Linear};
        QueryScoreDirection direction{QueryScoreDirection::Higher};
        double threshold{0.5}; /**< Step transition in the directed, normalized domain. */
    };

    /** @brief One mandatory compiled test may filter, score, or do both. */
    struct QueryScoreRule final {
        QueryStageId stage;
        std::optional<bool> requiredFilterValue;
        std::optional<QueryScoreRange> score;
    };

    /** @brief Outcome of the declared provider, translated at its typed adapter. */
    enum class QueryTestAvailability : std::uint8_t {
        Available,
        Unavailable,
        Failed,
        Incomparable,
        Count
    };

    /** @brief One provider's already evaluated facts. This carries no callback or mutation authority. */
    struct QueryTestSample final {
        QueryStageId stage;
        QueryTestAvailability availability{QueryTestAvailability::Available};
        std::optional<bool> filter;
        std::optional<double> metric;
    };

    /** @brief One bounded reason per candidate, identifying the first failed mandatory test in StageId order. */
    enum class QueryScoreStatus : std::uint8_t {
        Eligible,
        Filtered,
        MissingSample,
        ProviderUnavailable,
        ProviderFailed,
        Incomparable,
        InvalidScore,
    };

    /** @brief Stable ranking key and minimal diagnostic evidence. Rejected candidates never rank. */
    struct QueryCandidateScore final {
        std::uint32_t itemIndex{};
        std::uint32_t scoreKey{};
        std::int64_t tieBreakKey{};
        QueryScoreStatus status{QueryScoreStatus::MissingSample};
        QueryStageId failedStage;
        QueryProviderId failedProvider;
    };

    /** @brief Immutable policy joined to the exact descriptor-admitted tests of an EQS plan. */
    class QueryScoringPlan final {
    public:
        static constexpr std::uint32_t ScoreKeyScale = 1'000'000; /**< Version-one steps over the closed unit interval. */

        /**
         * @brief Validates and orders one rule per test stage, deriving provider identities from the admitted registry.
         * @param plan Admitted query stage inventory.
         * @param registry Exact registry used to compile the query.
         * @param rules Rules for all test stages; order has no semantic effect.
         * @param tieBreak Optional secondary rank key direction.
         * @return Owned scoring policy or a typed invalid, unavailable or duplicate rule failure.
         * @throws std::bad_alloc If the bounded stage table cannot be allocated.
         */
        [[nodiscard]] static Result<QueryScoringPlan> Capture(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                              std::span<const QueryScoreRule> rules, QueryScoreTieBreak tieBreak);

        /**
         * @brief Evaluates a candidate using only ordered test facts and the captured policy.
         * @param itemIndex Stable generator index within the hard candidate limit.
         * @param tieBreakKey Optional fixed-point secondary key captured by the caller.
         * @param samples Exactly one fact per stage in ascending StageId order.
         * @return Rankable score or the first typed stage/provider rejection.
         */
        [[nodiscard]] QueryCandidateScore Evaluate(std::uint32_t itemIndex, std::int64_t tieBreakKey,
                                                   std::span<const QueryTestSample> samples) const noexcept;

        /**
         * @brief Sorts caller-owned scores and returns a bounded prefix of eligible candidates.
         * @param candidates Mutable result scratch. Duplicate or out-of-range indexes are rejected before sorting.
         * @param bestCount Desired non-zero maximum winners.
         * @return A borrowed winning prefix or a typed limit/invalid failure.
         */
        [[nodiscard]] Result<std::span<const QueryCandidateScore>> SelectBest(std::span<QueryCandidateScore> candidates,
                                                                              std::size_t bestCount) const;

    private:
        struct Stage final {
            QueryScoreRule rule;
            QueryProviderId provider;
        };

        QueryScoringPlan() = default;
        std::vector<Stage> stages_;
        double totalWeight_{};
        std::uint32_t maximumResults_{};
        QueryScoreTieBreak tieBreak_{};
    };
}  // namespace Horo::AI
