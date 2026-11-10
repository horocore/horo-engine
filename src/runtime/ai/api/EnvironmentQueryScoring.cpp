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

        /** @brief Checks a mandatory provider reading before it enters numerical aggregation. */
        [[nodiscard]] QueryScoreStatus Status(const QueryTestSample &sample, const QueryScoreRule &rule) noexcept {
            using enum QueryTestAvailability;
            switch (sample.availability) {
                case Available:
                    break;
                case Unavailable:
                    return QueryScoreStatus::ProviderUnavailable;
                case Failed:
                    return QueryScoreStatus::ProviderFailed;
                case Incomparable:
                case Count:
                default:
                    return QueryScoreStatus::Incomparable;
            }
            if (rule.requiredFilterValue && !sample.filter)
                return QueryScoreStatus::MissingSample;
            if (rule.requiredFilterValue && rule.requiredFilterValue != sample.filter)
                return QueryScoreStatus::Filtered;
            if (rule.score && !sample.metric)
                return QueryScoreStatus::MissingSample;
            if (rule.score && !std::isfinite(*sample.metric))
                return QueryScoreStatus::InvalidScore;
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
            const auto stage = std::ranges::find(plan.Stages(), rule.stage, &QueryPlanStage::id);
            if (stage == plan.Stages().end() || !isTest(*stage))
                return CaptureResult::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            const auto *descriptor = registry.Find(stage->test);
            if (!descriptor)
                return CaptureResult::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            if ((!rule.requiredFilterValue && !rule.score) || (rule.score && !ValidRange(*rule.score)))
                return CaptureResult::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            captured.stages_.push_back({rule, descriptor->origin.provider});
        }
        std::ranges::sort(captured.stages_, {}, [](const Stage &stage) {
            return stage.rule.stage;
        });
        bool scored = false;
        QueryStageId previous;
        for (const auto &stage : captured.stages_) {
            if (stage.rule.stage == previous)
                return CaptureResult::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
            previous = stage.rule.stage;
            if (stage.rule.score) {
                scored = true;
                captured.totalWeight_ += stage.rule.score->weight;
            }
        }
        if (scored && (!std::isfinite(captured.totalWeight_) || captured.totalWeight_ <= 0))
            return CaptureResult::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
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
