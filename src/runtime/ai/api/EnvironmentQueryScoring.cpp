#include "Horo/AI/EnvironmentQueryScoring.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <bitset>
#include <cmath>

namespace Horo::AI {
    namespace {
        /** @brief Checks scoring coefficients before they can be used by any candidate. */
        [[nodiscard]] bool ValidRange(const QueryScoreRange &range) noexcept {
            const double width = range.maximum - range.minimum;
            return std::isfinite(width) && width > 0 && std::isfinite(range.weight) && range.weight >= 0 &&
                   range.curve < QueryScoreCurve::Count && range.direction < QueryScoreDirection::Count && std::isfinite(range.threshold) &&
                   range.threshold >= 0 && range.threshold <= 1;
        }

        /** @brief Converts a finite metric using only plan-declared coefficients. */
        [[nodiscard]] double WeightedScore(const QueryScoreRange &range, const double metric) noexcept {
            const double bounded = std::clamp(metric, range.minimum, range.maximum);
            double score = (bounded - range.minimum) / (range.maximum - range.minimum);
            if (range.direction == QueryScoreDirection::Lower)
                score = 1 - score;
            if (range.curve == QueryScoreCurve::SmoothStep)
                score = score * score * (3 - 2 * score);
            else if (range.curve == QueryScoreCurve::Threshold)
                score = score >= range.threshold ? 1 : 0;
            return score * range.weight;
        }

        /** @brief Translates the provider's availability without exposing runtime provider data. */
        [[nodiscard]] QueryScoreStatus ProviderStatus(const QueryTestAvailability availability) noexcept {
            using enum QueryTestAvailability;
            switch (availability) {
                case Available:
                    return QueryScoreStatus::Eligible;
                case Unavailable:
                    return QueryScoreStatus::ProviderUnavailable;
                case Failed:
                    return QueryScoreStatus::ProviderFailed;
                case Incomparable:
                case Count:
                default:
                    return QueryScoreStatus::Incomparable;
            }
        }

        /** @brief Checks a mandatory provider reading before it enters numerical aggregation. */
        [[nodiscard]] QueryScoreStatus Status(const QueryTestSample &sample, const QueryScoreRule &rule) noexcept {
            const auto available = ProviderStatus(sample.availability);
            if (available != QueryScoreStatus::Eligible)
                return available;
            if (rule.requiredFilterValue && rule.requiredFilterValue != sample.filter)
                return sample.filter ? QueryScoreStatus::Filtered : QueryScoreStatus::MissingSample;
            if (rule.score) {
                if (!sample.metric)
                    return QueryScoreStatus::MissingSample;
                if (!std::isfinite(*sample.metric))
                    return QueryScoreStatus::InvalidScore;
            }
            return QueryScoreStatus::Eligible;
        }

        /** @brief Orders scores without floating-point comparisons or enumeration-order dependence. */
        [[nodiscard]] bool RanksBefore(const QueryCandidateScore &left, const QueryCandidateScore &right,
                                       const QueryScoreTieBreak tieBreak) noexcept {
            const bool leftEligible = left.status == QueryScoreStatus::Eligible;
            const bool rightEligible = right.status == QueryScoreStatus::Eligible;
            if (leftEligible != rightEligible)
                return leftEligible;
            if (left.scoreKey != right.scoreKey)
                return left.scoreKey > right.scoreKey;
            if (left.tieBreakKey != right.tieBreakKey) {
                if (tieBreak == QueryScoreTieBreak::LowerKey)
                    return left.tieBreakKey < right.tieBreakKey;
                if (tieBreak == QueryScoreTieBreak::HigherKey)
                    return left.tieBreakKey > right.tieBreakKey;
            }
            return left.itemIndex < right.itemIndex;
        }
    }  // namespace

    /** @copydoc QueryScoringPlan::ResolveRule */
    Result<QueryScoringPlan::Stage> QueryScoringPlan::ResolveRule(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                                  const QueryScoreRule &rule) {
        const auto stage = std::ranges::find(plan.Stages(), rule.stage, &QueryPlanStage::id);
        if (stage == plan.Stages().end() || stage->kind != QueryStageKind::Test)
            return Result<Stage>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        const auto *descriptor = registry.Find(stage->test);
        if (!descriptor)
            return Result<Stage>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
        if ((!rule.requiredFilterValue && !rule.score) || (rule.score && !ValidRange(*rule.score)))
            return Result<Stage>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return Result<Stage>::Success({rule, descriptor->origin.provider});
    }

    /** @copydoc QueryScoringPlan::MeasureWeight */
    Result<double> QueryScoringPlan::MeasureWeight(const std::span<const Stage> stages) {
        bool scored = false;
        double weight = 0;
        QueryStageId previous;
        for (const auto &stage : stages) {
            if (stage.rule.stage == previous)
                return Result<double>::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
            previous = stage.rule.stage;
            if (stage.rule.score) {
                scored = true;
                weight += stage.rule.score->weight;
            }
        }
        if (scored && (!std::isfinite(weight) || weight <= 0))
            return Result<double>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        return Result<double>::Success(weight);
    }

    /** @copydoc QueryScoringPlan::Capture */
    Result<QueryScoringPlan> QueryScoringPlan::Capture(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                       const std::span<const QueryScoreRule> rules, const QueryScoreTieBreak tieBreak) {
        using CaptureResult = Result<QueryScoringPlan>;
        const auto isTest = [](const QueryPlanStage &stage) {
            return stage.kind == QueryStageKind::Test;
        };
        if (rules.size() != static_cast<std::size_t>(std::ranges::count_if(plan.Stages(), isTest)) || tieBreak >= QueryScoreTieBreak::Count)
            return CaptureResult::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        QueryScoringPlan captured;
        captured.maximumResults_ = plan.ResultSchema().maximumResults;
        captured.tieBreak_ = tieBreak;
        captured.stages_.reserve(rules.size());
        for (const auto &rule : rules) {
            const auto stage = ResolveRule(plan, registry, rule);
            if (stage.HasError())
                return CaptureResult::Failure(stage.ErrorValue());
            captured.stages_.push_back(stage.Value());
        }
        std::ranges::sort(captured.stages_, {}, [](const Stage &stage) {
            return stage.rule.stage;
        });
        const auto weight = MeasureWeight(captured.stages_);
        if (weight.HasError())
            return CaptureResult::Failure(weight.ErrorValue());
        captured.totalWeight_ = weight.Value();
        return CaptureResult::Success(std::move(captured));
    }

    /** @copydoc QueryScoringPlan::Evaluate */
    QueryCandidateScore QueryScoringPlan::Evaluate(const std::uint32_t itemIndex, const std::int64_t tieBreakKey,
                                                   const std::span<const QueryTestSample> samples) const noexcept {
        QueryCandidateScore result{.itemIndex = itemIndex, .tieBreakKey = tieBreakKey};
        if (itemIndex >= EnvironmentQuerySchemaLimits::MaximumResults || samples.size() > stages_.size())
            return result;
        double score = 0;
        for (std::size_t index = 0; index < stages_.size(); ++index) {
            const auto &stage = stages_[index];
            result.failedStage = stage.rule.stage;
            result.failedProvider = stage.provider;
            if (index >= samples.size() || samples[index].stage != stage.rule.stage) {
                result.status = QueryScoreStatus::MissingSample;
                return result;
            }
            const auto &sample = samples[index];
            result.status = Status(sample, stage.rule);
            if (result.status != QueryScoreStatus::Eligible)
                return result;
            if (stage.rule.score)
                score += WeightedScore(*stage.rule.score, *sample.metric);
            if (!std::isfinite(score)) {
                result.status = QueryScoreStatus::InvalidScore;
                return result;
            }
        }
        const double normalized = totalWeight_ > 0 ? std::clamp(score / totalWeight_, 0.0, 1.0) : 0;
        return {.itemIndex = itemIndex,
                .scoreKey = static_cast<std::uint32_t>(std::round(normalized * ScoreKeyScale)),
                .tieBreakKey = tieBreakKey,
                .status = QueryScoreStatus::Eligible};
    }

    /** @copydoc QueryScoringPlan::SelectBest */
    Result<std::span<const QueryCandidateScore>> QueryScoringPlan::SelectBest(const std::span<QueryCandidateScore> candidates,
                                                                              const std::size_t bestCount) const {
        using Selection = Result<std::span<const QueryCandidateScore>>;
        if (bestCount == 0 || bestCount > maximumResults_ || candidates.size() > EnvironmentQuerySchemaLimits::MaximumResults)
            return Selection::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        std::bitset<EnvironmentQuerySchemaLimits::MaximumResults> seen;
        for (const auto &candidate : candidates) {
            if (candidate.itemIndex >= EnvironmentQuerySchemaLimits::MaximumResults || candidate.scoreKey > ScoreKeyScale)
                return Selection::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            if (seen.test(candidate.itemIndex))
                return Selection::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
            seen.set(candidate.itemIndex);
        }
        std::ranges::sort(candidates, [this](const QueryCandidateScore &left, const QueryCandidateScore &right) {
            return RanksBefore(left, right, tieBreak_);
        });
        const auto prefix = candidates.first(std::min(bestCount, candidates.size()));
        const auto rejected = std::ranges::find_if_not(prefix, [](const QueryCandidateScore &score) {
            return score.status == QueryScoreStatus::Eligible;
        });
        return Selection::Success(prefix.first(static_cast<std::size_t>(rejected - prefix.begin())));
    }
}  // namespace Horo::AI
