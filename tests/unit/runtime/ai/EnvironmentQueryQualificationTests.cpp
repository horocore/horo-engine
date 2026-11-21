#include "AllocationProbe.h"
#include "EnvironmentQueryTestSupport.h"
#include "Horo/AI/EnvironmentQueryExecution.h"

#include <bit>
#include <chrono>
#include <cmath>

namespace Horo::AI {
    namespace {
        using namespace QueryTestSupport;

        constexpr std::size_t CandidateCount = EnvironmentQuerySchemaLimits::MaximumResults;
        constexpr std::size_t WinnerCount = 4;
        constexpr std::size_t TestUnits = 1;
        constexpr std::size_t ScoringUnits = 1;
        constexpr std::uint64_t SubmittedTick = 1;

        /** @brief Measures the normal owner transport with concrete points and the native distance kernel. */
        class DistanceWorkload final : public IOwnerQueryExecutionSource {
        public:
            DistanceWorkload() : IOwnerQueryExecutionSource({CandidateCount, TestUnits}) {
                points_.reserve(CandidateCount);
                for (std::size_t index = 0; index < CandidateCount; ++index)
                    points_.push_back(Math::WorldCoordinate64::FromMillimeters(static_cast<std::int64_t>(index) * 1'000, 0, 0));
            }

            bool IsCurrent(const QueryExecutionInputs &) const noexcept override {
                return true;
            }

        protected:
            Result<QueryGenerationResult> GenerateAtOwner(const QueryExecutionInputs &, const QueryGeneratorOutput &output,
                                                          AiWorkBudget &budget) noexcept override {
                (void)budget.Consume(points_.size());
                return GenerateQueryPoints(points_, output);
            }

            Result<QueryTestSample> TestAtOwner(const QueryExecutionInputs &, QueryStageId stage, const QueryGeneratedItem &item,
                                                AiWorkBudget &budget) noexcept override {
                (void)budget.Consume(TestUnits);
                return EvaluateQueryDistance(stage, QueryDistanceMetric::Euclidean, {}, std::get<Math::WorldCoordinate64>(item));
            }

        private:
            std::vector<Math::WorldCoordinate64> points_;
        };

        /** @brief Admits the same compiled plan for both inspection profiles. */
        std::unique_ptr<QueryExecution> AdmitWorkload(const Runtime::RuntimeSceneView &scene, const std::size_t debugSamples) {
            const QueryFixture fixture;
            const QueryTestDescriptor test{MakeIdentity<QueryTestId>(17),
                                           fixture.generator.origin,
                                           fixture.items.front().id,
                                           {1, 1},
                                           {},
                                           {}};
            const auto registry = fixture.Registry(std::span{&test, 1});
            const auto plan = fixture.Plan(registry, test.id);
            const std::array rules{QueryScoreRule{.stage = plan.Stages()[1].id,
                                                  .score = QueryScoreRange{.maximum = static_cast<double>(CandidateCount),
                                                                           .direction = QueryScoreDirection::Lower}}};
            auto scoring = QueryScoringPlan::Capture(plan, registry, rules, QueryScoreTieBreak::StableIndex);
            REQUIRE(scoring.HasValue());
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            QueryContextCapture capture;
            const auto contexts = capture.Capture(plan, registry, providers.Value(), scene, Source(scene));
            REQUIRE(contexts.HasValue());
            const QueryExecutionInputs inputs{{MakeIdentity<AiRuntimeIncarnation>(7), {1, 1}}, contexts.Value()};
            const QueryGeneratorProvenance provenance{plan.Stages()[0].id, fixture.generator.id, fixture.generator.origin.provider, 1};
            const QueryExecutionSettings settings{.submittedTick = SubmittedTick,
                                                  .deadlineTick = SubmittedTick + CandidateCount + 1,
                                                  .maximumCandidates = CandidateCount,
                                                  .bestCount = WinnerCount,
                                                  .generatorWorkUnits = CandidateCount,
                                                  .debugCapture = {debugSamples}};
            auto query = QueryExecution::AdmitDeterministic(inputs, provenance, std::move(scoring).Value(), settings,
                                                            std::make_unique<DistanceWorkload>());
            REQUIRE(query.HasValue());
            return std::move(query).Value();
        }

        struct WorkloadEvidence final {
            std::int64_t microseconds{};
            std::size_t allocations{};
            std::size_t workUnits{};
        };

        /** @brief Times only the admitted fixed-tick path and records actual work charges and allocations. */
        WorkloadEvidence ExecuteWorkload(QueryExecution &query) {
            constexpr std::size_t generationUnits = CandidateCount * (2 + std::bit_width(CandidateCount));
            constexpr std::size_t perCandidateUnits = TestUnits + ScoringUnits;
            using Clock = std::chrono::steady_clock;
            Tests::AllocationProbe::ScopedMeasurement allocation;
            const auto start = Clock::now();
            std::size_t workUnits{};
            for (std::uint64_t tick = SubmittedTick + 1; tick <= SubmittedTick + CandidateCount + 1; ++tick) {
                if (query.Sync(tick))
                    break;
                const auto available = perCandidateUnits + (tick == SubmittedTick + 1 ? generationUnits : 0);
                AiWorkBudget budget{available};
                query.Advance(tick, {budget, budget, budget});
                workUnits += available - budget.Remaining();
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
            return {.microseconds = elapsed, .allocations = allocation.Snapshot().requests, .workUnits = workUnits};
        }

        TEST_CASE("Deterministic EQS qualifies its maximum distance workload with and without capture", "[unit][ai][eqs][qualification]") {
            const auto scene = MakeScene();
            for (const auto samples : {std::size_t{0}, QueryDebugCaptureSettings::MaximumSamples}) {
                auto query = AdmitWorkload(scene->View(), samples);
                const auto evidence = ExecuteWorkload(*query);
                CAPTURE(samples, evidence.microseconds, evidence.workUnits, evidence.allocations);
                REQUIRE(query->Published());
                const auto &result = *query->Published();
                CHECK(result.status == QueryExecutionStatus::Completed);
                CHECK(result.mode == QueryExecutionMode::DeterministicWorkUnits);
                CHECK(result.publicationTick == SubmittedTick + CandidateCount + 1);
                CHECK(result.generated == CandidateCount);
                CHECK(result.evaluated == CandidateCount);
                REQUIRE(result.winners.size() == WinnerCount);
                for (std::size_t index = 0; index < WinnerCount; ++index) {
                    const auto &winner = result.winners[index];
                    const auto expected = 1 - static_cast<double>(index) / static_cast<double>(CandidateCount);
                    CHECK(winner.score.itemIndex == index);
                    CHECK(winner.score.scoreKey == static_cast<std::uint32_t>(std::round(expected * QueryScoringPlan::ScoreKeyScale)));
                }
                CHECK(evidence.workUnits == CandidateCount * (2 + std::bit_width(CandidateCount) + TestUnits + ScoringUnits));
                CHECK(evidence.allocations == 0);
                SUCCEED("Fixed-tick distance workload completed");
            }
        }
    }  // namespace
}  // namespace Horo::AI
