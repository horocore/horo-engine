#include "AllocationProbe.h"
#include "EnvironmentQueryTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryContexts.h"
#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using namespace QueryTestSupport;

        enum class SourceFailure {
            None,
            Generation,
            Admission,
            Poll,
            UnknownPoll,
            DiagnosticPoll,
            TypedAdmission
        };

        struct ExecutionProbe final {
            bool current{true};
            bool ready{true};
            bool backpressure{};
            bool overrun{};
            bool generationOverrun{};
            SourceFailure failure{SourceFailure::None};
            int generated{};
            int attempts{};
            int polls{};
            int cancelled{};
            QueryStageId lastStage;
        };

        class ExecutionSource final : public IQueryExecutionSource {
        public:
            explicit ExecutionSource(ExecutionProbe &probe) : probe_(probe) {}

            bool IsCurrent(const QueryExecutionInputs &) const noexcept override {
                return probe_.current;
            }

            Result<QueryGenerationResult> Generate(const QueryExecutionInputs &, const QueryGeneratorOutput &output) override {
                ++probe_.generated;
                if (probe_.failure == SourceFailure::Generation)
                    throw std::runtime_error("generator failed");
                const std::array points{Math::WorldCoordinate64::FromMillimeters(1, 0, 0),
                                        Math::WorldCoordinate64::FromMillimeters(2, 0, 0)};
                return GenerateQueryPoints(points, output);
            }

            Result<bool> StartTest(const QueryExecutionInputs &, const QueryStageId stage, const QueryGeneratedItem &item) override {
                ++probe_.attempts;
                if (probe_.failure == SourceFailure::Admission)
                    throw std::runtime_error("test admission failed");
                if (probe_.failure == SourceFailure::TypedAdmission)
                    return Result<bool>::Failure(MakeError(AIErrors::CapabilityUnavailable));
                probe_.lastStage = stage;
                if (probe_.backpressure)
                    return Result<bool>::Success(false);
                sample_ = {.stage = stage, .metric = static_cast<double>(std::get<Math::WorldCoordinate64>(item).Millimeters()[0]) / 2.0};
                return Result<bool>::Success(true);
            }

            Result<std::optional<QueryTestSample>> PollTest() override {
                ++probe_.polls;
                if (probe_.failure == SourceFailure::Poll)
                    throw std::runtime_error("poll failed");
                if (probe_.failure == SourceFailure::DiagnosticPoll) {
                    struct DiagnosticFailure final : std::exception {
                        const char *what() const noexcept override {
                            return "Provider polling failed with a native diagnostic that requires owned string allocation at the query "
                                   "boundary.";
                        }
                    };

                    throw DiagnosticFailure{};
                }
                if (probe_.failure == SourceFailure::UnknownPoll)
                    throw 42;
                return Result<std::optional<QueryTestSample>>::Success(probe_.ready ? std::exchange(sample_, std::nullopt) : std::nullopt);
            }

            void CancelTest() noexcept override {
                ++probe_.cancelled;
                sample_.reset();
            }

        private:
            ExecutionProbe &probe_;
            std::optional<QueryTestSample> sample_;
        };

        class OwnerSource final : public IOwnerQueryExecutionSource {
        public:
            explicit OwnerSource(ExecutionProbe &probe) : IOwnerQueryExecutionSource({2, 1}), probe_(probe) {}

            bool IsCurrent(const QueryExecutionInputs &) const noexcept override {
                return probe_.current;
            }

        protected:
            Result<QueryGenerationResult> GenerateAtOwner(const QueryExecutionInputs &, const QueryGeneratorOutput &output,
                                                          AiWorkBudget &budget) noexcept override {
                (void)budget.Consume(probe_.generationOverrun ? 3 : 2);
                ++probe_.generated;
                const std::array points{Math::WorldCoordinate64::FromMillimeters(1, 0, 0),
                                        Math::WorldCoordinate64::FromMillimeters(2, 0, 0)};
                return GenerateQueryPoints(points, output);
            }

            Result<QueryTestSample> TestAtOwner(const QueryExecutionInputs &, const QueryStageId stage, const QueryGeneratedItem &item,
                                                AiWorkBudget &budget) noexcept override {
                (void)budget.Consume(probe_.overrun ? 2 : 1);
                ++probe_.attempts;
                return Result<QueryTestSample>::Success(
                    {.stage = stage, .metric = static_cast<double>(std::get<Math::WorldCoordinate64>(item).Millimeters()[0]) / 2.0});
            }

        private:
            ExecutionProbe &probe_;
        };

        class ScheduledQuery final : public IAiScheduledDecision {
        public:
            explicit ScheduledQuery(QueryExecution &query) : query_(query) {}

            Result<AiSliceOutcome> Evaluate(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept override {
                AiWorkBudget queryBudget{10};
                query_.Advance(context.tick, {queryBudget, budget, budget});
                return Result<AiSliceOutcome>::Success(AiSliceOutcome::Complete);
            }

            Result<bool> Commit(const AiDecisionSliceContext &, AiWorkBudget &) noexcept override {
                return Result<bool>::Success(true);
            }

            void Cancel() noexcept override {
                query_.Cancel();
            }

        private:
            QueryExecution &query_;
        };

        [[nodiscard]] std::unique_ptr<QueryExecution> MakeExecution(ExecutionProbe &probe, Runtime::RuntimeScene &scene,
                                                                    bool reverseStages = false, bool deterministic = false) {
            QueryFixture fixture;
            const QueryTestDescriptor test{MakeIdentity<QueryTestId>(17), fixture.generator.origin, fixture.items[0].id, {1, 1}, {}, {}};
            const auto registry = fixture.Registry(std::span{&test, 1});
            const auto plan = fixture.Plan(registry, test.id, reverseStages);
            std::vector<QueryScoreRule> rules;
            for (const auto &stage : plan.Stages().subspan(1))
                rules.push_back({.stage = stage.id, .score = QueryScoreRange{}});
            auto scoring = QueryScoringPlan::Capture(plan, registry, rules, QueryScoreTieBreak::StableIndex);
            REQUIRE(scoring.HasValue());
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            QueryContextCapture capture;
            const auto view = scene.View();
            const auto contexts = capture.Capture(plan, registry, providers.Value(), view, Source(view));
            REQUIRE(contexts.HasValue());
            const QueryExecutionInputs inputs{{AiRuntimeIncarnation::Create(7).Value(), {1, 1}}, contexts.Value()};
            const QueryGeneratorProvenance provenance{plan.Stages()[0].id, fixture.generator.id, fixture.generator.origin.provider, 1};
            const auto admit = [&]() {
                if (deterministic)
                    return QueryExecution::AdmitDeterministic(inputs, provenance, std::move(scoring).Value(), {4, 8, 2, 2, 2, true},
                                                              std::make_unique<OwnerSource>(probe));
                return QueryExecution::Admit(inputs, provenance, std::move(scoring).Value(), {4, 8, 2, 2, 1, true},
                                             std::make_unique<ExecutionSource>(probe));
            };
            auto execution = admit();
            REQUIRE(execution.HasValue());
            return std::move(execution).Value();
        }

        TEST_CASE("Deterministic EQS uses real reserved owner scheduling and publishes the same semantic result tick",
                  "[unit][ai][eqs][execution][deterministic]") {
            for (const std::uint32_t workers : {0U, 1U}) {
                auto scene = MakeScene();
                ExecutionProbe probe;
                auto query = MakeExecution(probe, *scene, false, true);
                JobSystem jobs{{.workerCount = workers}};
                auto scheduler = AiTaskScheduler::Create(jobs, MakeIdentity<AiRuntimeIncarnation>(7),
                                                         {.maximumAgents = 1,
                                                          .evaluationsPerTick = 1,
                                                          .workUnitsPerTick = 10,
                                                          .commandsPerTick = 1,
                                                          .workerSubmissionsPerTick = 1,
                                                          .pendingWorkers = 1});
                REQUIRE(scheduler.HasValue());
                auto decision = std::make_shared<ScheduledQuery>(*query);
                REQUIRE(scheduler.Value()
                            ->Register({MakeIdentity<AiRuntimeIncarnation>(7), {1, 1}}, MakeIdentity<AgentId>(1),
                                       {.workUnitsPerTick = 10, .commandsPerTick = 1}, std::make_shared<int>(1), decision)
                            .HasValue());
                AiWorkBudget forbidden{100};
                query->Advance(4, {forbidden, forbidden, forbidden});
                CHECK(probe.generated == 0);
                for (std::uint64_t tick = 5; tick <= 7; ++tick) {
                    const auto terminal = query->Sync(tick);
                    CHECK(terminal == (tick == 7));
                    REQUIRE(scheduler.Value()->EvaluateAtDecision(tick).HasValue());
                    REQUIRE(scheduler.Value()->CommitAtIntentDispatch(tick).HasValue());
                }
                REQUIRE(query->Published());
                CHECK(query->Published()->mode == QueryExecutionMode::DeterministicWorkUnits);
                CHECK(query->Published()->publicationTick == 7);
                CHECK(query->Published()->status == QueryExecutionStatus::Completed);
                REQUIRE(query->Published()->winners.size() == 2);
                CHECK(query->Published()->winners[0].score.itemIndex == 1);
                CHECK(probe.generated == 1);
                CHECK(probe.attempts == 2);
            }
        }

        TEST_CASE("Deterministic EQS rejects skipped owner ticks and over-budget kernels without consuming late facts",
                  "[unit][ai][eqs][execution][deterministic]") {
            for (const bool overrun : {false, true}) {
                auto scene = MakeScene();
                ExecutionProbe probe;
                probe.overrun = overrun;
                auto query = MakeExecution(probe, *scene, false, true);
                CHECK_FALSE(query->Sync(5));
                AiWorkBudget budget{10};
                query->Advance(5, {budget, budget, budget});
                const std::uint64_t terminalTick = overrun ? 6 : 7;
                REQUIRE(query->Sync(terminalTick));
                REQUIRE(query->Published());
                CHECK(query->Published()->status == QueryExecutionStatus::Failed);
                REQUIRE(query->Published()->error);
                CHECK(query->Published()->error->code.Value() ==
                      (overrun ? AIErrors::SchedulerBudgetExhausted.code.Value() : AIErrors::SchedulerPhaseInvalid.code.Value()));
                CHECK(query->Published()->winners.empty());
                CHECK(query->Published()->publicationTick == terminalTick);
                CHECK(probe.attempts == 1);
                CHECK_FALSE(query->Sync(8));
            }
        }

        TEST_CASE("Deterministic EQS discards generation output that exceeds its declared kernel allowance",
                  "[unit][ai][eqs][execution][deterministic]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            probe.generationOverrun = true;
            auto query = MakeExecution(probe, *scene, false, true);
            CHECK_FALSE(query->Sync(5));
            AiWorkBudget budget{10};
            query->Advance(5, {budget, budget, budget});
            REQUIRE(query->Sync(6));
            REQUIRE(query->Published()->error);
            CHECK(query->Published()->error->code.Value() == AIErrors::SchedulerBudgetExhausted.code.Value());
            CHECK(query->Published()->status == QueryExecutionStatus::Failed);
            CHECK(query->Published()->winners.empty());
            CHECK(probe.attempts == 0);
        }

        TEST_CASE("Deterministic EQS cannot silently skip a reserved decision phase", "[unit][ai][eqs][execution][deterministic]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene, false, true);
            CHECK_FALSE(query->Sync(5));
            REQUIRE(query->Sync(6));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::Failed);
            REQUIRE(query->Published()->error);
            CHECK(query->Published()->error->code.Value() == AIErrors::SchedulerPhaseInvalid.code.Value());
            CHECK(probe.generated == 0);
        }

        TEST_CASE("EQS yields on shared budgets and reduces only completed work at the deadline", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene);
            AiWorkBudget queryBudget{9}, agentBudget{8}, globalBudget{100};
            CHECK_FALSE(query->Sync(4));
            query->Advance(4, {queryBudget, agentBudget, globalBudget});
            CHECK(probe.generated == 0);
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {queryBudget, agentBudget, globalBudget});
            CHECK(probe.generated == 1);
            CHECK(probe.attempts == 0);
            CHECK(queryBudget.Remaining() == 1);
            CHECK(agentBudget.Remaining() == 0);
            CHECK(globalBudget.Remaining() == 92);
            AiWorkBudget shared{5};
            query->Advance(5, {shared, shared, shared});
            CHECK(probe.attempts == 0);
            CHECK_FALSE(query->Sync(6));
            probe.backpressure = true;
            query->Advance(6, {shared, shared, shared});
            CHECK(shared.Remaining() == 4);
            CHECK_FALSE(query->Sync(7));
            CHECK(probe.polls == 0);
            probe.backpressure = false;
            query->Advance(7, {shared, shared, shared});
            REQUIRE(query->Sync(8));
            CHECK(probe.attempts == 2);
            CHECK(probe.polls == 1);
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::PartialSuccess);
            CHECK(query->Published()->evaluated == 1);
            REQUIRE(query->Published()->winners.size() == 1);
            CHECK(query->Published()->winners[0].score.itemIndex == 0);
            CHECK_FALSE(query->Sync(9));
        }

        TEST_CASE("EQS retains pending work and retires it exactly once on cancellation or staleness", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe{.ready = false};
            auto query = MakeExecution(probe, *scene);
            AiWorkBudget budget{9};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {budget, budget, budget});
            REQUIRE(probe.attempts == 1);
            CHECK_FALSE(query->Sync(6));
            AiWorkBudget empty{0};
            query->Advance(6, {empty, empty, empty});
            CHECK(probe.attempts == 1);
            CHECK(probe.polls == 1);
            QueryExecutionStatus expected{};
            SECTION("owner cancellation") {
                query->Cancel();
                query->Cancel();
                expected = QueryExecutionStatus::Cancelled;
            }
            SECTION("generation replacement") {
                probe.current = false;
                expected = QueryExecutionStatus::StaleInputs;
            }
            REQUIRE(query->Sync(7));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == expected);
            CHECK(query->Published()->winners.empty());
            CHECK(probe.cancelled == 1);
            CHECK(probe.polls == 1);
            CHECK_FALSE(query->Sync(8));
            query.reset();
            CHECK(probe.cancelled == 1);
        }

        TEST_CASE("EQS publishes completed tests in scored order", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene);
            AiWorkBudget first{9}, second{2};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {first, first, first});
            CHECK_FALSE(query->Sync(6));
            query->Advance(6, {second, second, second});
            REQUIRE(query->Sync(7));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::Completed);
            CHECK(query->Published()->contextRevision == 1);
            CHECK(query->Published()->scene == scene->View().RuntimeId());
            REQUIRE(query->Published()->winners.size() == 2);
            CHECK(query->Published()->winners[0].score.itemIndex == 1);
            CHECK(query->Published()->winners[1].score.itemIndex == 0);
            CHECK(probe.cancelled == 0);
        }

        TEST_CASE("EQS dispatches authored stage order and scores by stable stage identity", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene, true);
            AiWorkBudget first{10}, second{1};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {first, first, first});
            CHECK(probe.lastStage == MakeIdentity<QueryStageId>(18));
            CHECK_FALSE(query->Sync(6));
            query->Advance(6, {second, second, second});
            CHECK(probe.lastStage == MakeIdentity<QueryStageId>(16));
            REQUIRE(query->Sync(7, true));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::PartialSuccess);
            REQUIRE(query->Published()->winners.size() == 1);
            CHECK(query->Published()->winners[0].score.scoreKey == QueryScoringPlan::ScoreKeyScale / 2);
        }

        TEST_CASE("EQS isolates generator admission and poll exceptions from other queries", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            for (const auto point : {SourceFailure::Generation, SourceFailure::Admission, SourceFailure::Poll, SourceFailure::UnknownPoll,
                                     SourceFailure::TypedAdmission}) {
                ExecutionProbe failing{.failure = point}, healthy;
                auto rejected = MakeExecution(failing, *scene);
                auto accepted = MakeExecution(healthy, *scene);
                AiWorkBudget firstFailure{9}, firstHealthy{9}, secondHealthy{2};
                REQUIRE_FALSE(rejected->Sync(5));
                REQUIRE_FALSE(accepted->Sync(5));
                REQUIRE_NOTHROW(rejected->Advance(5, {firstFailure, firstFailure, firstFailure}));
                accepted->Advance(5, {firstHealthy, firstHealthy, firstHealthy});
                REQUIRE(rejected->Sync(6));
                REQUIRE(rejected->Published());
                CHECK(rejected->Published()->status == QueryExecutionStatus::Failed);
                CHECK(rejected->Published()->winners.empty());
                REQUIRE(rejected->Published()->error);
                const auto &expected =
                    point == SourceFailure::TypedAdmission ? AIErrors::CapabilityUnavailable : AIErrors::EnvironmentQueryProviderFailed;
                CHECK(rejected->Published()->error->code.Value() == expected.code.Value());
                CHECK(rejected->Published()->error->domain.Value() == expected.domain.Value());
                CHECK(failing.cancelled == ((point == SourceFailure::Poll || point == SourceFailure::UnknownPoll) ? 1 : 0));
                REQUIRE_FALSE(accepted->Sync(6));
                accepted->Advance(6, {secondHealthy, secondHealthy, secondHealthy});
                REQUIRE(accepted->Sync(7));
                CHECK(accepted->Published()->status == QueryExecutionStatus::Completed);
                CHECK(accepted->Published()->winners.size() == 2);
                CHECK_FALSE(rejected->Sync(7));
            }
        }

        TEST_CASE("EQS diagnostic allocation failure closes pending work and publishes one typed terminal", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            for (const auto point : {SourceFailure::DiagnosticPoll, SourceFailure::UnknownPoll}) {
                ExecutionProbe probe{.failure = point};
                auto query = MakeExecution(probe, *scene);
                AiWorkBudget first{9};
                REQUIRE_FALSE(query->Sync(5));
                query->Advance(5, {first, first, first});
                REQUIRE(probe.attempts == 1);
                const bool published = [&] {
                    Tests::AllocationProbe::ScopedFailure allocation;
                    return query->Sync(6);
                }();
                REQUIRE(published);
                const auto *result = query->Published();
                REQUIRE(result);
                CHECK(result->status == QueryExecutionStatus::Failed);
                CHECK(result->winners.empty());
                REQUIRE(result->error);
                CHECK(result->error->code.Value() == AIErrors::EnvironmentQueryProviderFailed.code.Value());
                CHECK(result->error->message.empty());
                CHECK(probe.cancelled == 1);
                query->Cancel();
                CHECK_FALSE(query->Sync(7));
                query.reset();
                CHECK(probe.cancelled == 1);
            }
        }

        TEST_CASE("EQS retires a pending provider when the publication window is missed", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene);
            AiWorkBudget first{9};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {first, first, first});
            REQUIRE(query->Sync(9));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::TimedOut);
            CHECK(probe.polls == 0);
            CHECK(probe.cancelled == 1);
        }

    }  // namespace
}  // namespace Horo::AI
