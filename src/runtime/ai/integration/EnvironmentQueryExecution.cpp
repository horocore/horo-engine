#include "Horo/AI/EnvironmentQueryExecution.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Bounds native diagnostic reads before constructing their owned representation. */
        std::string_view ExceptionText(const std::exception &error) noexcept {
            const char *text = error.what();
            if (!text)
                return {};
            std::size_t length{};
            while (length < 512 && text[length] != '\0')
                ++length;
            return {text, length};
        }

        /** @brief Contains diagnostic allocation failure within the original query-local provider failure. */
        [[nodiscard]] Error ProviderFailure() noexcept {
            const auto &descriptor = AIErrors::EnvironmentQueryProviderFailed;
            try {
                try {
                    throw;
                } catch (const std::exception &error) {
                    return MakeError(descriptor, std::string{ExceptionText(error)});
                } catch (...) {
                    return MakeError(descriptor);
                }
            } catch (const std::bad_alloc &) {
                // Filling MakeError's default summary here would attempt another allocation.
                return {.code = descriptor.code, .domain = descriptor.domain, .severity = descriptor.defaultSeverity};
            }
        }

        /** @brief Charges all distinct allowances together before work can start. */
        [[nodiscard]] bool Charge(const QueryExecutionBudget budget, const std::size_t units) noexcept {
            if (std::min({budget.query.Remaining(), budget.agent.Remaining(), budget.global.Remaining()}) < units)
                return false;
            (void)budget.query.Consume(units);
            if (&budget.agent != &budget.query)
                (void)budget.agent.Consume(units);
            if (&budget.global != &budget.query && &budget.global != &budget.agent)
                (void)budget.global.Consume(units);
            return true;
        }

        /** @brief Counts a bounded reduction reservation at admission to the generation stage. */
        [[nodiscard]] std::size_t GenerationCost(const QueryExecutionSettings &settings) noexcept {
            return settings.generatorWorkUnits + settings.maximumCandidates * (1 + std::bit_width(settings.maximumCandidates));
        }

        /** @brief Checks the captured execution identity and adapter before allocating query storage. */
        [[nodiscard]] bool ValidInputs(const QueryExecutionInputs &inputs, const QueryGeneratorProvenance &provenance,
                                       const QueryExecutionSettings &settings) noexcept {
            return inputs.agent.IsValid() && inputs.contexts && inputs.contexts->Scene().IsValid() && inputs.contexts->Revision() > 0 &&
                   provenance.stage.IsValid() && provenance.generator.IsValid() && provenance.provider.IsValid() &&
                   provenance.revision > 0 && settings.submittedTick < settings.deadlineTick;
        }

        /** @brief Checks finite generation, ranking and admission work bounds. */
        [[nodiscard]] bool WithinLimits(const QueryExecutionSettings &settings, const std::size_t maximumResults) noexcept {
            return settings.maximumCandidates > 0 && settings.maximumCandidates <= EnvironmentQuerySchemaLimits::MaximumResults &&
                   settings.bestCount > 0 && settings.bestCount <= maximumResults && settings.generatorWorkUnits > 0 &&
                   settings.generatorWorkUnits <= EnvironmentQuerySchemaLimits::MaximumResults * 16U;
        }

        /** @brief Rejects a generator that returns a different publication or more than its admitted capacity. */
        [[nodiscard]] bool MatchesGeneration(const QueryGenerationResult &generation, const QueryGeneratorOutput &output) noexcept {
            const auto &actual = generation.provenance;
            const auto &expected = output.provenance;
            return generation.itemCount <= output.maximumItems && actual.stage == expected.stage &&
                   actual.generator == expected.generator && actual.provider == expected.provider && actual.revision == expected.revision;
        }
    }  // namespace

    QueryExecution::QueryExecution(QueryExecutionInputs inputs, const QueryGeneratorProvenance provenance, QueryScoringPlan scoring,
                                   const QueryExecutionSettings settings, std::unique_ptr<IQueryExecutionSource> source)
        : inputs_(std::move(inputs)), scoring_(std::move(scoring)), settings_(settings), source_(std::move(source)),
          items_(settings.maximumCandidates), result_{.agent = inputs_.agent,
                                                      .scene = inputs_.contexts->Scene(),
                                                      .contextRevision = inputs_.contexts->Revision(),
                                                      .generator = provenance} {
        samples_.resize(scoring_.stages_.size());
        testOrder_.resize(samples_.size());
        std::iota(testOrder_.begin(), testOrder_.end(), std::size_t{0});
        std::ranges::sort(testOrder_, {}, [this](const std::size_t index) {
            return scoring_.stages_[index].executionOrder;
        });
        scores_.reserve(settings.maximumCandidates);
        result_.winners.reserve(std::min(settings.maximumCandidates, settings.bestCount));
    }

    /** @copydoc QueryExecution::Admit */
    Result<std::unique_ptr<QueryExecution>> QueryExecution::Admit(QueryExecutionInputs inputs, const QueryGeneratorProvenance provenance,
                                                                  QueryScoringPlan scoring, const QueryExecutionSettings settings,
                                                                  std::unique_ptr<IQueryExecutionSource> source) {
        using Admission = Result<std::unique_ptr<QueryExecution>>;
        if (!source || !ValidInputs(inputs, provenance, settings))
            return Admission::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        if (inputs.contexts->query_ != scoring.query_ || provenance.stage != scoring.generatorStage_ ||
            provenance.generator != scoring.generator_ || provenance.provider != scoring.generatorProvider_ ||
            scoring.tieBreak_ != QueryScoreTieBreak::StableIndex)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));
        if (!WithinLimits(settings, scoring.maximumResults_))
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        return Admission::Success(std::unique_ptr<QueryExecution>(
            new QueryExecution(std::move(inputs), provenance, std::move(scoring), settings, std::move(source))));
    }

    /** @copydoc QueryExecution::AdmitDeterministic */
    Result<std::unique_ptr<QueryExecution>> QueryExecution::AdmitDeterministic(QueryExecutionInputs inputs,
                                                                               const QueryGeneratorProvenance provenance,
                                                                               QueryScoringPlan scoring,
                                                                               const QueryExecutionSettings settings,
                                                                               std::unique_ptr<IOwnerQueryExecutionSource> source) {
        using Admission = Result<std::unique_ptr<QueryExecution>>;
        if (!source)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));
        const auto work = source->WorkLimits();
        if (work.generationUnits == 0 || (work.testUnits == 0 && !scoring.stages_.empty()) ||
            settings.generatorWorkUnits != work.generationUnits || work.testUnits > EnvironmentQuerySchemaLimits::MaximumResults * 16U)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        auto admitted = Admit(std::move(inputs), provenance, std::move(scoring), settings, std::move(source));
        if (admitted.HasValue()) {
            auto &execution = *admitted.Value();
            execution.result_.mode = QueryExecutionMode::DeterministicWorkUnits;

            execution.testWorkUnits_ = work.testUnits;
        }
        return admitted;
    }

    /** @copydoc QueryExecution::~QueryExecution */
    QueryExecution::~QueryExecution() {
        StopTest();
    }

    /** @copydoc QueryExecution::Cancel */
    void QueryExecution::Cancel() noexcept {
        if (!published_) {
            cancelled_ = true;
            StopTest();
        }
    }

    /** @brief Revokes accepted work before terminal publication or adapter destruction. */
    void QueryExecution::StopTest() noexcept {
        if (std::exchange(pending_, false))
            source_->CancelTest();
    }

    /** @brief Captures candidates once into preallocated storage. */
    void QueryExecution::Generate() {
        const QueryGeneratorOutput output{result_.generator, items_.size(), items_};
        auto generation = source_->Generate(inputs_, output);
        if (generation.HasError()) {
            result_.error = std::move(generation).ErrorValue();
            return;
        }
        if (!MatchesGeneration(generation.Value(), output)) {
            result_.error = MakeError(AIErrors::EnvironmentQuerySchemaInvalid);
            return;
        }
        result_.generated = generation.Value().itemCount;
        result_.invalidCoordinates = generation.Value().invalidCoordinateCount;
        generated_ = true;
    }

    /** @brief Scores one completed candidate against its reserved work allowance. */
    void QueryExecution::ScoreCandidate() {
        scores_.push_back(scoring_.Evaluate(static_cast<std::uint32_t>(scores_.size()), 0, samples_));
        nextTest_ = 0;
        scoringReserved_ = false;
    }

    /** @copydoc QueryExecution::Advance */
    void QueryExecution::Advance(const std::uint64_t tick, const QueryExecutionBudget budget) {
        if (!CanAdvance(tick))
            return;
        lastAdvance_ = tick;
        if (!source_->IsCurrent(inputs_)) {
            stale_ = true;
            return;
        }
        try {
            if (!generated_ && Charge(budget, GenerationCost(settings_)))
                Generate();
            if (generated_)
                AdvanceCandidates(budget);
        } catch (...) {
            StopTest();
            result_.error = ProviderFailure();
        }
    }

    /** @copydoc QueryExecution::CanAdvance */
    bool QueryExecution::CanAdvance(const std::uint64_t tick) const noexcept {
        return !published_ && !cancelled_ && !result_.error && !pending_ && tick == lastSync_ && tick > lastAdvance_;
    }

    /** @copydoc QueryExecution::AdvanceCandidates */
    void QueryExecution::AdvanceCandidates(const QueryExecutionBudget budget) {
        const auto scoringCost = std::max<std::size_t>(1, scoring_.stages_.size());
        while (scores_.size() < result_.generated) {
            if (!scoringReserved_ && !Charge(budget, scoringCost))
                return;
            scoringReserved_ = true;
            if (scoring_.stages_.empty()) {
                ScoreCandidate();
                continue;
            }
            if (!Charge(budget, testWorkUnits_))
                return;
            auto admitted = source_->StartTest(inputs_, scoring_.stages_[testOrder_[nextTest_]].rule.stage, items_[scores_.size()]);
            if (admitted.HasError())
                result_.error = std::move(admitted).ErrorValue();
            else
                pending_ = admitted.Value();
            return;
        }
    }

    /** @brief Drains one provider result and rejects facts for a different stage. */
    void QueryExecution::Poll() {
        try {
            auto completion = source_->PollTest();
            if (completion.HasError()) {
                pending_ = false;
                result_.error = std::move(completion).ErrorValue();
                return;
            }
            if (!completion.Value())
                return;
            pending_ = false;
            const auto &sample = *completion.Value();
            const auto slot = testOrder_[nextTest_];
            if (sample.stage != scoring_.stages_[slot].rule.stage) {
                result_.error = MakeError(AIErrors::EnvironmentQuerySchemaInvalid);
                return;
            }
            samples_[slot] = sample;
            if (++nextTest_ == testOrder_.size())
                ScoreCandidate();
        } catch (...) {
            StopTest();
            result_.error = ProviderFailure();
        }
    }

    /** @copydoc QueryExecution::Sync */
    bool QueryExecution::Sync(const std::uint64_t tick, const bool finishEarly) {
        if (published_ || tick <= settings_.submittedTick || tick <= lastSync_)
            return false;
        ValidateTickSequence(tick);
        lastSync_ = tick;
        if (cancelled_)
            Publish(QueryExecutionStatus::Cancelled);
        else if (stale_ || !source_->IsCurrent(inputs_))
            Publish(QueryExecutionStatus::StaleInputs);
        else if (const auto status = TerminalAt(tick, finishEarly))
            Publish(*status);
        return published_;
    }

    /** @copydoc QueryExecution::ValidateTickSequence */
    void QueryExecution::ValidateTickSequence(const std::uint64_t tick) {
        const bool skippedSync = tick != (lastSync_ == 0 ? settings_.submittedTick : lastSync_) + 1;
        const bool skippedDecision = lastSync_ > settings_.submittedTick && lastAdvance_ != lastSync_;
        if (result_.mode == QueryExecutionMode::DeterministicWorkUnits && (skippedSync || skippedDecision)) {
            StopTest();
            result_.error = MakeError(AIErrors::SchedulerPhaseInvalid);
        }
    }

    /** @copydoc QueryExecution::TerminalAt */
    std::optional<QueryExecutionStatus> QueryExecution::TerminalAt(const std::uint64_t tick, const bool finishEarly) {
        const bool deadlineReached = tick >= settings_.deadlineTick;
        if (pending_ && tick <= settings_.deadlineTick)
            Poll();
        if (result_.error)
            return QueryExecutionStatus::Failed;
        if (generated_ && scores_.size() == result_.generated)
            return QueryExecutionStatus::Completed;
        if (!finishEarly && !deadlineReached)
            return std::nullopt;
        if (settings_.allowPartial)
            return QueryExecutionStatus::PartialSuccess;
        return deadlineReached ? QueryExecutionStatus::TimedOut : QueryExecutionStatus::Failed;
    }

    /** @copydoc QueryExecution::ReduceWinners */
    QueryExecutionStatus QueryExecution::ReduceWinners(const QueryExecutionStatus status) {
        auto selected = scoring_.SelectBest(scores_, settings_.bestCount);
        if (selected.HasError()) {
            result_.error = std::move(selected).ErrorValue();
            return QueryExecutionStatus::Failed;
        }
        for (const auto &score : selected.Value())
            result_.winners.push_back({items_[score.itemIndex], score});
        if (!result_.winners.empty())
            return status;
        if (status == QueryExecutionStatus::PartialSuccess)
            return lastSync_ >= settings_.deadlineTick ? QueryExecutionStatus::TimedOut : QueryExecutionStatus::Failed;
        const bool invalidScore = std::ranges::any_of(scores_, [](const auto &score) {
            return score.status == QueryScoreStatus::InvalidScore;
        });
        return invalidScore ? QueryExecutionStatus::Failed : status;
    }

    /** @brief Reduces the completed candidate prefix and publishes exactly once. */
    void QueryExecution::Publish(QueryExecutionStatus status) {
        StopTest();
        result_.evaluated = scores_.size();
        if (status == QueryExecutionStatus::Completed || status == QueryExecutionStatus::PartialSuccess)
            status = ReduceWinners(status);
        if (status != QueryExecutionStatus::Failed)
            result_.error.reset();
        result_.status = status;
        result_.publicationTick = lastSync_;
        published_ = true;
    }

    /** @copydoc QueryExecution::Published */
    const QueryExecutionResult *QueryExecution::Published() const noexcept {
        return published_ ? &result_ : nullptr;
    }
}  // namespace Horo::AI
