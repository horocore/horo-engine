#include "Horo/AI/EnvironmentQueryExecution.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryCache.h"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
                    return MakeError(descriptor, std::string{""});
                }
            } catch (const std::bad_alloc &) {
                // Filling MakeError's default summary here would attempt another allocation.
                return {.code = descriptor.code,
                        .domain = descriptor.domain,
                        .severity = descriptor.defaultSeverity,
                        .message = std::string{""},
                        .diagnostics = std::vector<Diagnostic>(std::size_t{0})};
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
                   settings.generatorWorkUnits <= EnvironmentQuerySchemaLimits::MaximumResults * 16U &&
                   settings.debugCapture.maximumSamples <= QueryDebugCaptureSettings::MaximumSamples && settings.testWorkUnits > 0 &&
                   settings.testWorkUnits <= EnvironmentQuerySchemaLimits::MaximumResults * 16U;
        }

        /** @brief Rejects a generator that returns a different publication or more than its admitted capacity. */
        [[nodiscard]] bool MatchesGeneration(const QueryGenerationResult &generation, const QueryGeneratorOutput &output) noexcept {
            const auto &actual = generation.provenance;
            const auto &expected = output.provenance;
            return generation.itemCount <= output.maximumItems && actual.stage == expected.stage &&
                   actual.generator == expected.generator && actual.provider == expected.provider && actual.revision == expected.revision;
        }

        /** @brief Runs bounded point stages through one generation pass and one outstanding test reservation. */
        class PointProviderSource final : public IQueryExecutionSource {
        public:
            explicit PointProviderSource(std::span<PreparedPointQueryTest> tests) : count_(tests.size()) {
                for (std::size_t index = 0; index < count_; ++index) {
                    stages_[index] = tests[index].rule.stage;
                    sources_[index] = std::move(tests[index].source);
                }
            }

            bool IsCurrent(const QueryExecutionInputs &inputs) const noexcept override {
                return std::ranges::all_of(std::span{sources_}.first(count_), [&](const auto &source) {
                    return source->IsCurrent(inputs);
                });
            }

            Result<QueryGenerationResult> Generate(const QueryExecutionInputs &inputs, const QueryGeneratorOutput &output) override {
                return sources_.front()->Generate(inputs, output);
            }

            Result<bool> StartTest(const QueryExecutionInputs &inputs, QueryStageId stage, const QueryGeneratedItem &item) override {
                const auto active = std::span{stages_}.first(count_);
                const auto found = std::ranges::find(active, stage);
                if (pending_ || found == active.end())
                    return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
                auto &source = *sources_[static_cast<std::size_t>(found - active.begin())];
                auto admitted = source.StartTest(inputs, stage, item);
                if (admitted.HasValue() && admitted.Value())
                    pending_ = &source;
                return admitted;
            }

            Result<std::optional<QueryTestSample>> PollTest() override {
                if (!pending_)
                    return Result<std::optional<QueryTestSample>>::Success(std::nullopt);
                auto sample = pending_->PollTest();
                if (sample.HasError() || sample.Value())
                    pending_ = nullptr;
                return sample;
            }

            void CancelTest() noexcept override {
                if (auto *source = std::exchange(pending_, nullptr))
                    source->CancelTest();
            }

        private:
            static constexpr auto MaximumTests = EnvironmentQuerySchemaLimits::Stages - 1;
            std::size_t count_{};
            std::array<QueryStageId, MaximumTests> stages_{};
            std::array<std::unique_ptr<IQueryExecutionSource>, MaximumTests> sources_;
            IQueryExecutionSource *pending_{};
        };

        /** @brief Checks only the exact captured generation. Replacement actors cannot satisfy this read. */
        [[nodiscard]] bool RetiredActor(const QueryGeneratedItem &item, const Runtime::RuntimeSceneView &scene) {
            const auto *actor = std::get_if<Runtime::EntityRef>(&item);
            return actor && scene.Get(*actor).HasError();
        }

        /** @brief Projects only the bounded winning prefix and checks its captured actor generations. */
        void CopyDebugWinners(QueryDebugSnapshot &snapshot, const QueryExecutionResult &result, const std::size_t limit,
                              const Runtime::RuntimeSceneView &scene) {
            snapshot.terminalStatus = result.status;
            snapshot.failure = result.failure;
            snapshot.winnerCount = result.winners.size();
            const auto count = std::min(snapshot.winnerCount, limit);
            snapshot.winners.reserve(count);
            for (const auto &winner : std::span{result.winners}.first(count)) {
                snapshot.stale = snapshot.stale || RetiredActor(winner.item, scene);
                snapshot.winners.push_back(winner.score);
            }
        }
    }  // namespace

    struct QueryExecution::CacheBinding final {
        Runtime::RuntimeSceneView scene;
        std::weak_ptr<QueryResultCache> cache;
        Sha256Digest artifact;
        std::array<std::byte, 5> parameters;

        [[nodiscard]] QueryCacheKey Key(QueryId query) const noexcept {
            // Native lattice and distance kernels only read the cooked plan and captured point contexts.
            return {query, artifact, parameters, {}, true};
        }
    };

    QueryExecution::QueryExecution(QueryExecutionInputs inputs, const QueryGeneratorProvenance provenance, QueryScoringPlan scoring,
                                   const QueryExecutionSettings settings, std::unique_ptr<IQueryExecutionSource> source)
        : inputs_(std::move(inputs)), scoring_(std::move(scoring)), settings_(settings), source_(std::move(source)),
          items_(settings.maximumCandidates), result_{.agent = inputs_.agent,
                                                      .scene = inputs_.contexts->Scene(),
                                                      .contextRevision = inputs_.contexts->Revision(),
                                                      .sourceContext = inputs_.contexts,
                                                      .generator = provenance} {
        samples_.resize(scoring_.stages_.size());
        testOrder_.resize(samples_.size());
        std::iota(testOrder_.begin(), testOrder_.end(), std::size_t{0});
        std::ranges::sort(testOrder_, {}, [this](const std::size_t index) {
            return scoring_.stages_[index].executionOrder;
        });
        scores_.reserve(settings.maximumCandidates);
        result_.winners.reserve(std::min(settings.maximumCandidates, settings.bestCount));
        if (settings.debugCapture.maximumSamples != 0) {
            const auto count = std::min(settings.debugCapture.maximumSamples, settings.maximumCandidates);
            debug_ = std::make_unique<QueryDebugSnapshot>();
            debug_->query = inputs_.contexts->Query();
            debug_->generator = provenance.stage;
            debug_->submittedTick = settings.submittedTick;
            debug_->candidates.reserve(count);
            debug_->stages.reserve(count * scoring_.stages_.size());
        }
    }

    /** @copydoc AdmitPreparedPointQuery */
    Result<std::unique_ptr<QueryExecution>> AdmitPreparedPointQuery(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                                                    QueryExecutionInputs inputs, QueryExecutionSettings settings,
                                                                    std::span<PreparedPointQueryTest> tests) {
        using Admission = Result<std::unique_ptr<QueryExecution>>;
        if (tests.empty() || tests.size() >= EnvironmentQuerySchemaLimits::Stages)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));
        const auto generator = tests.front().generator;
        settings.generatorWorkUnits = tests.front().generationWorkUnits;
        settings.testWorkUnits = tests.front().testWorkUnits;
        std::array<QueryScoreRule, EnvironmentQuerySchemaLimits::Stages - 1> rules;
        for (std::size_t index = 0; index < tests.size(); ++index) {
            const auto &test = tests[index];
            if (!test.source || test.generator.stage != generator.stage || test.generator.generator != generator.generator ||
                test.generator.provider != generator.provider || test.generator.revision != generator.revision)
                return Admission::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            settings.maximumCandidates = std::min(settings.maximumCandidates, test.maximumCandidates);
            settings.testWorkUnits = std::max(settings.testWorkUnits, test.testWorkUnits);
            rules[index] = test.rule;
        }
        auto scoring = QueryScoringPlan::Capture(plan, registry, std::span{rules}.first(tests.size()), QueryScoreTieBreak::StableIndex);
        if (scoring.HasError())
            return Admission::Failure(scoring.ErrorValue());
        auto source = tests.size() == 1 ? std::move(tests.front().source) : std::make_unique<PointProviderSource>(tests);
        return QueryExecution::Admit(std::move(inputs), generator, std::move(scoring).Value(), settings, std::move(source));
    }

    /** @copydoc QueryExecution::Admit */
    Result<std::unique_ptr<QueryExecution>> QueryExecution::Admit(QueryExecutionInputs inputs, const QueryGeneratorProvenance provenance,
                                                                  QueryScoringPlan scoring, const QueryExecutionSettings settings,
                                                                  std::unique_ptr<IQueryExecutionSource> source) {
        using Admission = Result<std::unique_ptr<QueryExecution>>;
        if (!source || !ValidInputs(inputs, provenance, settings))
            return Admission::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        if (inputs.ownerCancellation.IsCancellationRequested())
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        if (inputs.contexts->query_ != scoring.query_ || provenance.stage != scoring.generatorStage_ ||
            provenance.generator != scoring.generator_ || provenance.provider != scoring.generatorProvider_ ||
            scoring.tieBreak_ != QueryScoreTieBreak::StableIndex)
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryStageUnsupported));
        if (!WithinLimits(settings, scoring.maximumResults_))
            return Admission::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        auto execution = std::unique_ptr<QueryExecution>(
            new QueryExecution(std::move(inputs), provenance, std::move(scoring), settings, std::move(source)));
        execution->testWorkUnits_ = settings.testWorkUnits;
        return Admission::Success(std::move(execution));
    }

    /** @copydoc QueryExecution::AdmitDeterministic */
    Result<std::unique_ptr<QueryExecution>> QueryExecution::AdmitDeterministic(QueryExecutionInputs inputs,
                                                                               const QueryGeneratorProvenance provenance,
                                                                               QueryScoringPlan scoring,
                                                                               const QueryExecutionSettings settings,
                                                                               std::unique_ptr<IOwnerQueryExecutionSource> source) {
        return AdmitOwner(std::move(inputs), provenance, std::move(scoring), settings, std::move(source),
                          QueryExecutionMode::DeterministicWorkUnits);
    }

    Result<std::unique_ptr<QueryExecution>> QueryExecution::AdmitOwner(QueryExecutionInputs inputs,
                                                                       const QueryGeneratorProvenance provenance, QueryScoringPlan scoring,
                                                                       const QueryExecutionSettings settings,
                                                                       std::unique_ptr<IOwnerQueryExecutionSource> source,
                                                                       const QueryExecutionMode mode) {
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
            execution.result_.mode = mode;
            execution.testWorkUnits_ = work.testUnits;
        }
        return admitted;
    }

    void QueryExecution::BindPointCache(const Runtime::RuntimeSceneView &scene, const BuiltinPointQueryReuse &reuse) {
        if (debug_ || reuse.artifact == Sha256Digest{})
            return;
        static_assert(EnvironmentQuerySchemaLimits::MaximumResults <= 0xffffU);
        const auto candidates = settings_.maximumCandidates;
        const auto winners = settings_.bestCount;
        const std::array parameters{std::byte{1}, std::byte(candidates >> 8U), std::byte(candidates), std::byte(winners >> 8U),
                                    std::byte(winners)};
        cache_ = std::make_unique<CacheBinding>(CacheBinding{scene, reuse.cache, reuse.artifact, parameters});
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

    void QueryExecution::RecordGenerationFailure(Error error) {
        result_.failure = QueryFailureDiagnostic{.stage = result_.generator.stage,
                                                 .provider = result_.generator.provider,
                                                 .reason = QueryScoreStatus::ProviderFailed,
                                                 .phase = QueryFailurePhase::Generation};
        result_.error = std::move(error);
    }

    void QueryExecution::RecordTestFailure(const std::size_t slot, const QueryFailurePhase phase, Error error) {
        const auto &stage = scoring_.stages_[slot];
        const auto reason = error.code.Value() == AIErrors::CapabilityUnavailable.code.Value() ? QueryScoreStatus::ProviderUnavailable
                                                                                               : QueryScoreStatus::ProviderFailed;
        result_.failure =
            QueryFailureDiagnostic{stage.rule.stage, stage.provider, static_cast<std::uint32_t>(scores_.size()), reason, phase};
        result_.error = std::move(error);
    }

    /** @brief Captures candidates once into preallocated storage. */
    void QueryExecution::Generate() {
        const QueryGeneratorOutput output{result_.generator, items_.size(), items_};
        auto generation = source_->Generate(inputs_, output);
        if (generation.HasError()) {
            RecordGenerationFailure(std::move(generation).ErrorValue());
            return;
        }
        if (!MatchesGeneration(generation.Value(), output)) {
            RecordGenerationFailure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            return;
        }
        result_.generated = generation.Value().itemCount;
        result_.invalidCoordinates = generation.Value().invalidCoordinateCount;
        generated_ = true;
        if (debug_)
            CaptureDebugCandidates();
    }

    /** @copydoc QueryExecution::CaptureDebugCandidates */
    void QueryExecution::CaptureDebugCandidates() {
        const auto count = std::min(result_.generated, settings_.debugCapture.maximumSamples);
        for (std::size_t sample = 0; sample < count; ++sample) {
            const auto index = sample * (result_.generated - 1) / std::max<std::size_t>(1, count - 1);
            const auto *point = std::get_if<Math::WorldCoordinate64>(&items_[index]);
            QueryDebugCandidate candidate{.index = static_cast<std::uint32_t>(index), .actor = point == nullptr};
            if (point && settings_.debugCapture.includeWorldPoints)
                candidate.point = *point;
            debug_->candidates.push_back(candidate);
        }
    }

    /** @copydoc QueryExecution::CurrentDebugCandidate */
    QueryDebugCandidate *QueryExecution::CurrentDebugCandidate() noexcept {
        if (!debug_ || debugSample_ >= debug_->candidates.size())
            return nullptr;
        auto &candidate = debug_->candidates[debugSample_];
        return candidate.index == scores_.size() ? &candidate : nullptr;
    }

    /** @brief Scores one completed candidate against its reserved work allowance. */
    void QueryExecution::ScoreCandidate() {
        const auto score = scoring_.Evaluate(static_cast<std::uint32_t>(scores_.size()), 0, samples_);
        if (auto *candidate = CurrentDebugCandidate()) {
            candidate->score = score;
            ++debugSample_;
        }
        if (score.failedProvider.IsValid() &&
            (!result_.failure || (result_.failure->reason == QueryScoreStatus::Filtered && score.status != QueryScoreStatus::Filtered) ||
             (result_.failure->reason != QueryScoreStatus::InvalidScore && score.status == QueryScoreStatus::InvalidScore))) {
            result_.failure =
                QueryFailureDiagnostic{score.failedStage, score.failedProvider, score.itemIndex, score.status, QueryFailurePhase::Scoring};
        }
        scores_.push_back(score);
        nextTest_ = 0;
        scoringReserved_ = false;
    }

    /** @copydoc QueryExecution::Advance */
    void QueryExecution::Advance(const std::uint64_t tick, const QueryExecutionBudget budget) {
        if (!CanAdvance(tick))
            return;
        lastAdvance_ = tick;
        if (StopIfInvalidated())
            return;
        try {
            if (!generated_ && Charge(budget, GenerationCost(settings_))) {
                Generate();
                if (StopIfInvalidated())
                    return;
            }
            if (generated_) {
                AdvanceCandidates(budget);
                if (pending_)
                    (void)StopIfInvalidated();
            }
        } catch (...) {
            StopTest();
            if (!generated_)
                RecordGenerationFailure(ProviderFailure());
            else
                result_.error = ProviderFailure();
        }
    }

    /** @copydoc QueryExecution::CanAdvance */
    bool QueryExecution::CanAdvance(const std::uint64_t tick) const noexcept {
        return !published_ && !cancelled_ && !result_.error && !pending_ && tick == lastSync_ && tick > lastAdvance_;
    }

    /** @brief Gives cancellation precedence when the adapter revokes authority during its input check. */
    std::optional<QueryExecutionStatus> QueryExecution::InputInvalidation() const noexcept {
        if (cancelled_ || inputs_.ownerCancellation.IsCancellationRequested())
            return QueryExecutionStatus::Cancelled;
        const bool current = !stale_ && source_->IsCurrent(inputs_);
        if (inputs_.ownerCancellation.IsCancellationRequested())
            return QueryExecutionStatus::Cancelled;
        return current ? std::nullopt : std::optional{QueryExecutionStatus::StaleInputs};
    }

    /** @brief Retires provider work immediately and preserves the terminal reason for BlackboardSync. */
    bool QueryExecution::StopIfInvalidated() noexcept {
        const auto reason = InputInvalidation();
        if (!reason)
            return false;
        if (*reason == QueryExecutionStatus::Cancelled)
            Cancel();
        else {
            stale_ = true;
            StopTest();
        }
        return true;
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
            StartCandidateTest();
            return;
        }
    }

    void QueryExecution::StartCandidateTest() {
        const auto slot = testOrder_[nextTest_];
        try {
            auto admitted = source_->StartTest(inputs_, scoring_.stages_[slot].rule.stage, items_[scores_.size()]);
            if (admitted.HasError())
                RecordTestFailure(slot, QueryFailurePhase::TestStart, std::move(admitted).ErrorValue());
            else
                pending_ = admitted.Value();
        } catch (...) {
            RecordTestFailure(slot, QueryFailurePhase::TestStart, ProviderFailure());
        }
    }

    /** @brief Drains one provider result and rejects facts for a different stage. */
    void QueryExecution::Poll() {
        const auto slot = testOrder_[nextTest_];
        try {
            auto completion = source_->PollTest();
            if (completion.HasError()) {
                pending_ = false;
                RecordTestFailure(slot, QueryFailurePhase::TestCompletion, std::move(completion).ErrorValue());
                return;
            }
            if (!completion.Value())
                return;
            pending_ = false;
            const auto &sample = *completion.Value();
            if (sample.stage != scoring_.stages_[slot].rule.stage) {
                RecordTestFailure(slot, QueryFailurePhase::TestCompletion, MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                return;
            }
            samples_[slot] = sample;
            if (const auto *candidate = CurrentDebugCandidate())
                debug_->stages.push_back({candidate->index, scoring_.DescribeStage(slot, sample)});
            if (++nextTest_ == testOrder_.size())
                ScoreCandidate();
        } catch (...) {
            StopTest();
            RecordTestFailure(slot, QueryFailurePhase::TestCompletion, ProviderFailure());
        }
    }

    /** @copydoc QueryExecution::Sync */
    bool QueryExecution::Sync(const std::uint64_t tick, const bool finishEarly) {
        if (published_ || tick <= settings_.submittedTick || tick <= lastSync_)
            return false;
        const bool firstSync = lastSync_ == 0;
        ValidateTickSequence(tick);
        lastSync_ = tick;
        if (const auto reason = InputInvalidation())
            Publish(*reason);
        else if (firstSync && tick <= settings_.deadlineTick && TryPublishCached())
            return true;
        else if (const auto status = TerminalAt(tick, finishEarly))
            Publish(*status);
        return published_;
    }

    bool QueryExecution::TryPublishCached() {
        const auto cache = cache_ ? cache_->cache.lock() : nullptr;
        if (!cache)
            return false;
        const auto retained = cache->Lookup(cache_->scene, inputs_, *source_, cache_->Key(inputs_.contexts->Query()), lastSync_);
        if (const auto reason = InputInvalidation()) {
            Publish(*reason);
            return true;
        }
        if (!retained)
            return false;
        // The lookup has revalidated source authority. Keep the new submission's provenance and publication tick.
        result_.generated = retained->generated;
        result_.evaluated = retained->evaluated;
        result_.invalidCoordinates = retained->invalidCoordinates;
        result_.failure = retained->failure;
        result_.winners.assign(retained->winners.begin(), retained->winners.end());
        result_.status = QueryExecutionStatus::Completed;
        result_.publicationTick = lastSync_;
        published_ = true;
        cache_.reset();
        return true;
    }

    /** @copydoc QueryExecution::ValidateTickSequence */
    void QueryExecution::ValidateTickSequence(const std::uint64_t tick) {
        const bool skippedSync = tick != (lastSync_ == 0 ? settings_.submittedTick : lastSync_) + 1;
        const bool skippedDecision = lastSync_ > settings_.submittedTick && lastAdvance_ != lastSync_;
        if (result_.mode == QueryExecutionMode::DeterministicWorkUnits && (skippedSync || skippedDecision)) {
            StopTest();
            result_.failure.reset();
            result_.error = MakeError(AIErrors::SchedulerPhaseInvalid);
        }
    }

    /** @copydoc QueryExecution::TerminalAt */
    std::optional<QueryExecutionStatus> QueryExecution::TerminalAt(const std::uint64_t tick, const bool finishEarly) {
        const bool deadlineReached = tick >= settings_.deadlineTick;
        if (pending_ && tick <= settings_.deadlineTick) {
            Poll();
            if (const auto reason = InputInvalidation())
                return reason;
        }
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
            result_.failure.reset();
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
        if (!result_.winners.empty() || (status != QueryExecutionStatus::Completed && status != QueryExecutionStatus::Failed))
            result_.failure.reset();
        result_.status = status;
        result_.publicationTick = lastSync_;
        published_ = true;
        if (const auto cache = cache_ ? cache_->cache.lock() : nullptr)
            (void)cache->Store(cache_->scene, inputs_, *source_, cache_->Key(inputs_.contexts->Query()), result_, lastSync_);
        cache_.reset();
    }

    /** @copydoc QueryExecution::Published */
    const QueryExecutionResult *QueryExecution::Published() const noexcept {
        return published_ ? &result_ : nullptr;
    }

    /** @copydoc QueryExecution::ReadDebugSnapshot */
    std::optional<QueryDebugSnapshot> QueryExecution::ReadDebugSnapshot(const Runtime::RuntimeSceneView &scene) const {
        if (!debug_)
            return std::nullopt;
        auto snapshot = *debug_;
        snapshot.observedTick = std::max(lastSync_, settings_.submittedTick);
        snapshot.generated = result_.generated;
        snapshot.evaluated = scores_.size();
        snapshot.invalidCoordinates = result_.invalidCoordinates;
        snapshot.stale = stale_ || !inputs_.contexts->MatchesScene(scene) || !InputsAreCurrent() ||
                         (published_ && result_.status == QueryExecutionStatus::StaleInputs);
        snapshot.stale = snapshot.stale || std::ranges::any_of(snapshot.candidates, [&](const QueryDebugCandidate &candidate) {
            return RetiredActor(items_[candidate.index], scene);
        });
        if (published_)
            CopyDebugWinners(snapshot, result_, settings_.debugCapture.maximumSamples, scene);
        if (snapshot.stale)
            for (auto &candidate : snapshot.candidates)
                candidate.point.reset();
        return snapshot;
    }

    /** @copydoc QueryExecution::MatchesAdmission */
    bool QueryExecution::MatchesAdmission(const QueryId query, const AgentHandle agent, const std::uint64_t submittedTick,
                                          const std::uint64_t deadlineTick) const noexcept {
        return inputs_.contexts->Query() == query && inputs_.agent == agent && settings_.submittedTick == submittedTick &&
               settings_.deadlineTick == deadlineTick;
    }

    /** @copydoc QueryExecution::InputsAreCurrent */
    bool QueryExecution::InputsAreCurrent() const noexcept {
        return !InputInvalidation();
    }
}  // namespace Horo::AI
