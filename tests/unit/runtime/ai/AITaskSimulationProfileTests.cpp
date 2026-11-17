#include "AITaskSchedulerTestSupport.h"
#include "BehaviorTreeAsyncTestSupport.h"

namespace Horo::AI {
    namespace {
        using SchedulerTestSupport::Harness;
        using SchedulerTestSupport::Probe;
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        /** @brief Uses distinct host-authored boundaries to exercise all policy states. */
        AiTaskSchedulerSettings Settings() {
            return {.mode = AiSchedulingMode::BestEffort,
                    .simulationProfile = AiDecisionSimulationProfile{.fullDistanceMillimeters = 10'000,
                                                                     .reducedDistanceMillimeters = 20'000,
                                                                     .suspensionDistanceMillimeters = 30'000,
                                                                     .reducedIntervalTicks = 3,
                                                                     .demotionDelayTicks = 2}};
        }

        /** @brief Runs the same owner phases used by Scene scheduling. */
        std::size_t EvaluateTick(AiTaskScheduler &scheduler, const std::uint64_t tick) {
            const auto decision = scheduler.EvaluateAtDecision(tick);
            REQUIRE(decision.HasValue());
            REQUIRE(scheduler.CommitAtIntentDispatch(tick).HasValue());
            return decision.Value().evaluated;
        }

        /** @brief Reads the exact live registration after an owner tick. */
        AiSimulationTier Tier(AiTaskScheduler &scheduler, const AgentHandle agent) {
            const auto tier = scheduler.SimulationTier(agent);
            REQUIRE(tier.HasValue());
            return tier.Value();
        }

        TEST_CASE("AI simulation profiles require increasing bands and best-effort scheduling", "[unit][ai][simulation-tier]") {
            Harness fixture;
            auto settings = Settings();
            SECTION("deterministic mode rejects reduced simulation") {
                settings.mode = AiSchedulingMode::Deterministic;
            }
            SECTION("equal bands are invalid") {
                settings.simulationProfile->reducedDistanceMillimeters = settings.simulationProfile->fullDistanceMillimeters;
            }
            SECTION("reduced cadence must differ from full rate") {
                settings.simulationProfile->reducedIntervalTicks = 1;
            }
            SECTION("demotion must have positive hysteresis") {
                settings.simulationProfile->demotionDelayTicks = 0;
            }
            ExpectError(AiTaskScheduler::Create(fixture.jobs, fixture.incarnation, settings), AIErrors::SchedulerPolicyInvalid);
        }

        TEST_CASE("AI stable relevance lowers cadence without renderer data and wakes from gameplay events",
                  "[unit][ai][simulation-tier]") {
            Harness fixture(Settings());
            const auto agent = fixture.Handle(0);
            const auto probe = fixture.Register(0);
            auto &scheduler = *fixture.scheduler;
            auto stale = agent;
            ++stale.slot.generation;
            ExpectError(scheduler.SetRelevanceAtSafePoint(stale, {}), AIErrors::TaskContextInvalid);
            REQUIRE(scheduler.SetRelevanceAtSafePoint(agent, {.anchorDistanceMillimeters = 15'000}).HasValue());
            CHECK(EvaluateTick(scheduler, 1) == 1);
            CHECK(Tier(scheduler, agent) == AiSimulationTier::Full);
            REQUIRE(scheduler.SetRelevanceAtSafePoint(agent, {.anchorDistanceMillimeters = 10'000}).HasValue());
            CHECK(EvaluateTick(scheduler, 2) == 1);

            REQUIRE(scheduler.SetRelevanceAtSafePoint(agent, {.anchorDistanceMillimeters = 20'000}).HasValue());
            CHECK(EvaluateTick(scheduler, 3) == 1);
            CHECK(EvaluateTick(scheduler, 4) == 1);
            CHECK(Tier(scheduler, agent) == AiSimulationTier::Full);
            CHECK(EvaluateTick(scheduler, 5) == 0);
            CHECK(Tier(scheduler, agent) == AiSimulationTier::Reduced);

            REQUIRE(scheduler.Wake(agent, {.polling = true}).HasValue());
            REQUIRE(
                scheduler.SetRelevanceAtSafePoint(agent, {.anchorDistanceMillimeters = 31'000, .activeSimulationRegion = true}).HasValue());
            CHECK(EvaluateTick(scheduler, 6) == 0);
            CHECK(EvaluateTick(scheduler, 7) == 1);
            CHECK(probe->reasons.polling);
            CHECK(EvaluateTick(scheduler, 8) == 0);
            CHECK(Tier(scheduler, agent) == AiSimulationTier::EventOnly);
            REQUIRE(scheduler.Wake(agent, {.polling = true}).HasValue());
            CHECK(EvaluateTick(scheduler, 9) == 0);
            REQUIRE(scheduler.Wake(agent, {.blackboard = true}).HasValue());
            CHECK(EvaluateTick(scheduler, 10) == 1);
            CHECK(probe->reasons.blackboard);

            REQUIRE(scheduler.SetRelevanceAtSafePoint(agent, {.anchorDistanceMillimeters = 31'000, .urgentStimulus = true}).HasValue());
            CHECK(EvaluateTick(scheduler, 11) == 1);
            CHECK(Tier(scheduler, agent) == AiSimulationTier::Full);
            CHECK(probe->cancelled == 0);
        }

        TEST_CASE("Suspended Scene decision drops optional work and preserves canonical agent state",
                  "[unit][ai][simulation-tier][scene]") {
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
            TestSupport::RestoreHarness scene;
            auto settings = Settings();
            settings.simulationProfile->demotionDelayTicks = 1;
            REQUIRE(scene.runtime.ConfigureTaskSchedulerAtSafePoint(jobs, settings).HasValue());
            auto image = std::make_shared<int>(1);
            auto probe = std::make_shared<Probe>();
            probe->remaining = 4;
            REQUIRE(scene.runtime.RegisterDecisionAtSafePoint(scene.handle, {.commandsPerTick = 1}, image, probe).HasValue());
            const auto borrowed = scene.runtime.TaskSchedulerAtSafePoint();
            REQUIRE(borrowed.HasValue());
            auto &scheduler = *borrowed.Value();
            const auto capability = scene.runtime.CapabilityAtSafePoint();
            const auto key = scene.fixtures.front().schema->Keys().front().key;
            auto batch = capability.BeginBlackboardWrite(scene.handle);
            REQUIRE(batch.HasValue());
            auto write = std::move(batch).Value();
            REQUIRE(write.Stage({key, BlackboardScalarValue{true}}).HasValue());
            REQUIRE(capability.CommitBlackboardWrite(scene.handle, std::move(write)).HasValue());
            const auto canonical = scene.Capture();

            const AiDecisionRelevance distant{.anchorDistanceMillimeters = 31'000};
            REQUIRE(scheduler.SetRelevanceAtSafePoint(scene.handle, distant).HasValue());
            CHECK(EvaluateTick(scheduler, 1) == 1);
            CHECK(probe->committed == 1);
            CHECK(probe->intents == 3);
            CHECK(EvaluateTick(scheduler, 2) == 0);
            CHECK(Tier(scheduler, scene.handle) == AiSimulationTier::Suspended);
            CHECK(probe->cancelled == 1);
            CHECK(probe->committed == 1);
            CHECK(probe->intents == 0);

            REQUIRE(scheduler.Wake(scene.handle, {.blackboard = true, .task = true}).HasValue());
            CHECK(EvaluateTick(scheduler, 3) == 0);
            CHECK(probe->cancelled == 1);
            CHECK(scene.Capture() == canonical);
            const auto current = capability.ResolveAgent(scene.fixtures.front().agent.agent.agent);
            REQUIRE(current.HasValue());
            CHECK(current.Value() == scene.handle);

            REQUIRE(scheduler.Wake(scene.handle, {.perception = true}).HasValue());
            CHECK(EvaluateTick(scheduler, 4) == 1);
            CHECK(Tier(scheduler, scene.handle) == AiSimulationTier::EventOnly);
            CHECK(probe->reasons.perception);
            REQUIRE(
                scheduler.SetRelevanceAtSafePoint(scene.handle, {.anchorDistanceMillimeters = 31'000, .gameplayPinned = true}).HasValue());
            CHECK(EvaluateTick(scheduler, 5) == 1);
            CHECK(Tier(scheduler, scene.handle) == AiSimulationTier::Full);
            CHECK(scene.Capture() == canonical);
        }

        /** @brief Finishes before the next owner phase, leaving only a pending scheduler publication. */
        struct CompletedWorker final : IAiBudgetedTaskJob {
            CompletedWorker() : IAiBudgetedTaskJob(1) {}

            Result<void> RunSlice(const AiTaskOperationContext &, const CancellationToken &, AiWorkBudget &budget) const override {
                if (!budget.Consume())
                    return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
                return Result<void>::Success();
            }
        };

        TEST_CASE("Suspending AI fences completed worker publication before pumping mailboxes", "[unit][ai][simulation-tier][jobs]") {
            BehaviorTreeAsyncTestSupport::StartedTask task;
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
            const auto continuation = task.async->continuations[1];
            const auto agent = continuation.Operation().agent;
            auto settings = Settings();
            settings.simulationProfile->demotionDelayTicks = 1;
            auto created = AiTaskScheduler::Create(jobs, agent.incarnation, settings);
            REQUIRE(created.HasValue());
            auto scheduler = std::move(created).Value();
            auto image = std::make_shared<int>(1);
            auto probe = std::make_shared<Probe>();
            probe->scheduler = scheduler.get();
            probe->continuation = continuation;
            probe->worker.emplace(image, std::make_shared<CompletedWorker>());
            REQUIRE(scheduler->Register(agent, MakeIdentity<AgentId>(1), {}, image, probe).HasValue());
            REQUIRE(scheduler->SetRelevanceAtSafePoint(agent, {.anchorDistanceMillimeters = 31'000}).HasValue());
            const auto initial = scheduler->EvaluateAtDecision(1);
            REQUIRE(initial.HasValue());
            CHECK(initial.Value().workerSubmissions == 1);
            ExpectError(scheduler->SetRelevanceAtSafePoint(agent, {}), AIErrors::SchedulerPhaseInvalid);
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());
            jobs.Shutdown(ShutdownPolicy::Drain);

            CHECK(EvaluateTick(*scheduler, 2) == 0);
            CHECK(Tier(*scheduler, agent) == AiSimulationTier::Suspended);
            CHECK(probe->cancelled == 1);
            CHECK(task.fixture.Evaluate(*task.instance, 2) == AiTaskState::Running);
        }
    }  // namespace
}  // namespace Horo::AI
