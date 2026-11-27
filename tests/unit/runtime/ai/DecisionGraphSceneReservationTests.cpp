#include "AiSceneTestSupport.h"
#include "AllocationProbe.h"
#include "DecisionGraphCompilerTestSupport.h"
#include "Horo/AI/AIReplayEvidence.h"
#include "Horo/AI/DecisionQueryTask.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "PerceptionSenseTestSupport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/generators/catch_generators.hpp>
#include <thread>

namespace Horo::AI {
    namespace {
        using namespace DecisionGraphTestSupport;

        /** @brief Builds one scored action against the real scalar schema used by its Scene owner. */
        DecisionGraphSource UtilitySource() {
            const auto key = MakeIdentity<BlackboardKeyId>(101);
            UtilityDecisionSource body;
            body.root = Node(1);
            body.requirements = {{.key = key, .kind = BlackboardValueKind::Scalar, .source = Location()}};
            UtilityDecisionOption option;
            option.option = {Node(2), MakeIdentity<TaskId>(5002), 1, 0, {{.key = key}}};
            option.provider = {.id = Node(2), .type = Type(501), .descriptorVersion = {1, 1}, .source = Location()};
            body.options.push_back(std::move(option));
            return {Graph(1), 1, SchemaId(), {1, 1}, Location(), {}, std::move(body)};
        }

        /** @brief Publishes the exact compiler schema and bindings through the production Scene owner. */
        TestSupport::ActivationFixture SceneFixture(const Fixture &fixture, const CookedDecisionPlan &plan) {
            auto scene = TestSupport::MakeFixture(1, 11, 7, 0);
            scene.schema = fixture.schema;
            scene.plan = plan.Graphs()[plan.Occurrences()[0].plan].bindings;
            scene.descriptor.blackboardSchema = scene.schema;
            scene.descriptor.decisionPlan = scene.plan;
            scene.descriptor.decisionAsset = plan.Root();
            scene.descriptor.decisionKind = scene.plan->Kind();
            scene.agent.controller->blackboardSchema = scene.schema->Identity();
            scene.agent.controller->decisionAsset = plan.Root();
            scene.agent.controller->decisionKind = scene.plan->Kind();
            return scene;
        }

        /** @brief Feeds the real scheduler's allowance and exact Scene snapshot to the cooked evaluator. */
        class ScheduledTreeProbe final : public IAiScheduledDecision {
        public:
            ScheduledTreeProbe(const AiSceneRuntime &runtime, std::unique_ptr<DecisionGraphInstance> tree)
                : runtime_(runtime), tree_(std::move(tree)) {}

            Result<AiSliceOutcome> Evaluate(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept override {
                const auto snapshot = runtime_.CaptureDecisionSnapshotAtSafePoint(context.agent);
                if (snapshot.HasError())
                    return Result<AiSliceOutcome>::Failure(snapshot.ErrorValue());
                const auto stepped = tree_->StepBudgeted({context.tick, snapshot.Value().Binding(), snapshot.Value(), {}}, budget);
                if (stepped.HasError())
                    return Result<AiSliceOutcome>::Failure(stepped.ErrorValue());
                return Result<AiSliceOutcome>::Success(stepped.Value() ? AiSliceOutcome::Complete : AiSliceOutcome::BudgetExhausted);
            }

            Result<bool> Commit(const AiDecisionSliceContext &, AiWorkBudget &) noexcept override {
                return Result<bool>::Success(true);
            }

            void Cancel() noexcept override {
                static_cast<void>(tree_->Cancel(AiTaskCancellationReason::AgentGenerationRetired));
            }

        private:
            const AiSceneRuntime &runtime_;
            std::unique_ptr<DecisionGraphInstance> tree_;
        };

        TEST_CASE("Scene scheduler grants a complete cooked tree slice before starting its real provider task",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            const std::array sources{TaskTree(11)};
            const auto compiled = fixture.Compile(sources, 11);
            const auto encoded = DecisionPlanCodec::EncodeCooked(*compiled);
            REQUIRE(encoded.HasValue());
            auto decoded = DecisionPlanCodec::DecodeCooked(encoded.Value(), fixture.Environment());
            REQUIRE(decoded.HasValue());
            const auto plan = std::move(decoded).Value().plan;
            REQUIRE(plan);
            const auto required = plan->MaximumDecisionWorkUnits();
            REQUIRE(required);
            REQUIRE(*required > 1);

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            auto publication = TestSupport::Publish(runtime, scene);
            REQUIRE(runtime.ConfigureTaskSchedulerAtSafePoint(jobs).HasValue());
            auto scheduler = runtime.TaskSchedulerAtSafePoint();
            REQUIRE(scheduler.HasValue());
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            const auto taskDefinition = TestSupport::MakeIdentity<TaskId>(9000);
            const auto parent = runtime.StartTaskAtSafePoint(agent, taskDefinition);
            REQUIRE(parent.HasValue());
            auto reservation = runtime.ReserveDecisionTaskSlotsAtSafePoint(agent, static_cast<std::uint32_t>(plan->Storage().taskSlots));
            REQUIRE(reservation.HasValue());
            auto tree = DecisionGraphInstance::Create(plan, fixture.providers, std::move(reservation).Value(),
                                                      {taskDefinition, parent.Value(), agent});
            REQUIRE(tree.HasValue());

            const auto snapshot = runtime.CaptureDecisionSnapshotAtSafePoint(agent);
            REQUIRE(snapshot.HasValue());
            AiWorkBudget insufficient{*required - 1};
            DecisionStepTrace deniedEvidence;
            const auto denied =
                tree.Value()->StepBudgetedWithTrace({1, snapshot.Value().Binding(), snapshot.Value(), {}}, insufficient, deniedEvidence);
            REQUIRE(denied.HasValue());
            CHECK_FALSE(denied.Value());
            CHECK(deniedEvidence.Header().tick == 0);
            CHECK(deniedEvidence.Records().empty());
            CHECK(insufficient.Exhausted());
            CHECK(insufficient.Remaining() == *required - 1);
            CHECK(fixture.probe.starts == 0);

            bool admitted = true;
            SECTION("scheduler allocates the full bound and evaluates the same untouched tick") {}
            SECTION("scheduler reports exhaustion without starting, resuming or publishing a task") {
                admitted = false;
            }
            const AiAgentSchedulePolicy policy{.workUnitsPerTick = admitted ? *required : *required - 1};
            auto executor = std::make_shared<ScheduledTreeProbe>(runtime, std::move(tree).Value());
            REQUIRE(runtime.RegisterDecisionAtSafePoint(agent, policy, fixture.providerOwner, executor).HasValue());
            const auto report = scheduler.Value()->EvaluateAtDecision(1);
            REQUIRE(report.HasValue());
            CHECK(report.Value().failed == 0);
            CHECK(report.Value().exhausted == (admitted ? 0 : 1));
            CHECK(fixture.probe.starts == (admitted ? 1 : 0));
            CHECK(fixture.probe.resumes == 0);
            REQUIRE(scheduler.Value()->CommitAtIntentDispatch(1).HasValue());
            REQUIRE(runtime.DisableAtSafePoint(agent).HasValue());
            CHECK(fixture.probe.aborts == (admitted ? 1 : 0));
            CHECK(fixture.probe.cleanups == (admitted ? 1 : 0));
        }

        TEST_CASE("Scene budgets State Machine calls and retires their active descendants on replacement",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            auto root = CallTree(1, 2, DecisionPlanKind::StateMachine);
            root.executionTask = MakeIdentity<TaskId>(9000);
            const std::array sources{root, CallingMachine(2, 3, DecisionPlanKind::BehaviorTree), TaskTree(3)};
            const auto plan = fixture.Compile(sources, 1);
            const auto allowance = plan->MaximumDecisionWorkUnits();
            REQUIRE(allowance.has_value());
            auto source = TaskTree(1);
            source.executionTask = root.executionTask;
            const auto replacement = fixture.Compile(std::span{&source, 1}, 1);
            REQUIRE(*allowance > replacement->MaximumDecisionWorkUnits().value());

            const auto graph = fixture.Instance(plan);
            const auto board = fixture.blackboard->Snapshot().Value();
            AiWorkBudget shortBudget{*allowance - 1};
            const auto denied = graph->StepBudgeted({1, fixture.binding, board, {}}, shortBudget);
            REQUIRE(denied.HasValue());
            CHECK_FALSE(denied.Value().has_value());
            CHECK(shortBudget.Remaining() == *allowance - 1);
            CHECK(fixture.probe.starts == 0);

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {.workUnitsPerTick = *allowance}}).Value();
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            REQUIRE(scheduler->EvaluateAtDecision(1).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());
            REQUIRE(fixture.probe.starts == 1);
            const auto retired = fixture.probe.continuation;
            REQUIRE(retired.IsCurrent());
            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, plan, replacement).HasValue());
            CHECK_FALSE(retired.IsCurrent());
            REQUIRE(fixture.probe.abortReasons.size() == 1);
            CHECK(fixture.probe.abortReasons.front() == AiTaskCancellationReason::PlanReplaced);
            CHECK(fixture.probe.cleanups == 1);
            CHECK(fixture.probe.starts == 1);
            REQUIRE(scheduler->EvaluateAtDecision(2).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());
            CHECK(fixture.probe.starts == 2);
            CHECK(fixture.probe.resumes == 0);
        }

        TEST_CASE("A suspended cooked decision resumes under the same Scene agent generation",
                  "[ai][decision_reservation][simulation-tier]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            auto root = CallTree(1, 2, DecisionPlanKind::StateMachine);
            root.executionTask = MakeIdentity<TaskId>(9000);
            const std::array sources{root, CallingMachine(2, 3, DecisionPlanKind::BehaviorTree), TaskTree(3)};
            const auto plan = fixture.Compile(sources, 1);
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            const AiTaskSchedulerSettings settings{.mode = AiSchedulingMode::BestEffort,
                                                   .simulationProfile = AiDecisionSimulationProfile{10, 20, 30, 2, 1}};
            const AiSceneCookedDecisionExecution execution{.jobs = jobs,
                                                          .scheduler = settings,
                                                          .agent = {.workUnitsPerTick = plan->MaximumDecisionWorkUnits().value()},
                                                          .timing = AiCookedDecisionTiming::BestEffortReactive};
            auto runtime = AiSceneRuntime::Create({}, execution).Value();
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            const auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            const auto run = [&](std::uint64_t tick) {
                const auto evaluated = scheduler->EvaluateAtDecision(tick);
                REQUIRE(evaluated.HasValue());
                CHECK(evaluated.Value().failed == 0);
                REQUIRE(scheduler->CommitAtIntentDispatch(tick).HasValue());
            };
            REQUIRE(scheduler->SetRelevanceAtSafePoint(agent, {.anchorDistanceMillimeters = 31}).HasValue());
            run(1);
            REQUIRE(fixture.probe.starts == 1);
            const auto retired = fixture.probe.continuation;
            REQUIRE(retired.IsCurrent());
            const auto before = runtime.CaptureDecisionSnapshotAtSafePoint(agent).Value().Binding();
            run(2);
            CHECK(scheduler->SimulationTier(agent).Value() == AiSimulationTier::Suspended);
            CHECK_FALSE(retired.IsCurrent());
            CHECK(fixture.probe.cleanups == 1);
            CHECK(fixture.probe.abortReasons == std::vector{AiTaskCancellationReason::Superseded});
            REQUIRE(scheduler->SetRelevanceAtSafePoint(agent, {.gameplayPinned = true}).HasValue());
            run(3);
            CHECK(scheduler->SimulationTier(agent).Value() == AiSimulationTier::Full);
            REQUIRE(fixture.probe.starts == 2);
            CHECK(fixture.probe.resumes == 0);
            CHECK(fixture.probe.tasks[0] != fixture.probe.tasks[1]);
            CHECK(runtime.CaptureDecisionSnapshotAtSafePoint(agent).Value().Binding() == before);
        }

        TEST_CASE("Scene admits the entire tree closure and retires its removed callee at a tick boundary",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture old;
            Fixture next;
            old.probe.succeedTick = 100;
            next.probe.succeedTick = 100;
            next.schema = old.schema;
            auto root = CallTree(11, 12, DecisionPlanKind::BehaviorTree);
            root.executionTask = MakeIdentity<TaskId>(9000);
            const std::array sources{root, TaskTree(12)};
            const auto previous = old.Compile(sources, 11);
            auto nextRoot = TaskTree(11);
            nextRoot.executionTask = root.executionTask;
            const auto replacement = next.Compile(std::span{&nextRoot, 1}, 11);
            const auto allowance = previous->MaximumDecisionWorkUnits();
            REQUIRE(allowance.has_value());
            REQUIRE(*allowance > replacement->MaximumDecisionWorkUnits().value());

            auto standalone = old.Instance(previous);
            const auto board = old.blackboard->Snapshot().Value();
            AiWorkBudget shortBudget{*allowance - 1};
            const auto denied = standalone->StepBudgeted({1, old.binding, board, {}}, shortBudget);
            REQUIRE(denied.HasValue());
            CHECK_FALSE(denied.Value().has_value());
            CHECK(shortBudget.Remaining() == *allowance - 1);
            CHECK(old.probe.starts == 0);

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {.workUnitsPerTick = *allowance}}).Value();
            auto scene = SceneFixture(old, *previous);
            scene.descriptor.cookedPlan = previous;
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            REQUIRE(scheduler->EvaluateAtDecision(1).Value().failed == 0);
            const auto continuation = old.probe.continuation;
            REQUIRE(continuation.IsCurrent());
            TestSupport::ExpectError(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement),
                                     AIErrors::SchedulerPhaseInvalid);
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());

            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement).HasValue());
            CHECK_FALSE(continuation.IsCurrent());
            REQUIRE(old.probe.abortReasons.size() == 1);
            CHECK(old.probe.abortReasons.front() == AiTaskCancellationReason::PlanReplaced);
            CHECK(old.probe.cleanups == 1);
            CHECK(next.probe.starts == 0);
            TestSupport::ExpectError(scheduler->EvaluateAtDecision(1), AIErrors::SchedulerPhaseInvalid);
            REQUIRE(scheduler->EvaluateAtDecision(2).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());
            CHECK(next.probe.starts == 1);
            CHECK(old.probe.resumes == 0);
        }

        TEST_CASE("Private Scene activates its cooked scheduler only after scoped host admission",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            auto source = TaskTree(11);
            source.executionTask = MakeIdentity<TaskId>(9000);
            const auto plan = fixture.Compile(std::span{&source, 1}, 11);
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            CHECK(fixture.probe.factories == 0);
            CHECK(runtime.TaskSchedulerAtSafePoint().HasError());
            TestSupport::ExpectError(runtime.ActivateCookedDecisionsAtSafePoint({jobs, {}, {.workUnitsPerTick = 1}}),
                                     AIErrors::SchedulerBudgetExhausted);
            CHECK(runtime.TaskSchedulerAtSafePoint().HasError());
            CHECK(fixture.probe.factories == 0);

            REQUIRE(runtime.ActivateCookedDecisionsAtSafePoint({jobs, {}, {}}).HasValue());
            CHECK(fixture.probe.factories == 1);
            CHECK(runtime.ActivateCookedDecisionsAtSafePoint({jobs, {}, {}}).HasError());
            const auto next = runtime.ReserveDecisionTaskSlotsAtSafePoint(agent, 1);
            REQUIRE(next.HasValue());
            CHECK(next.Value().FirstSlot() == plan->Storage().taskSlots + 1);
            const auto scheduler = runtime.TaskSchedulerAtSafePoint();
            REQUIRE(scheduler.HasValue());
            REQUIRE(scheduler.Value()->EvaluateAtDecision(1).HasValue());
            REQUIRE(scheduler.Value()->CommitAtIntentDispatch(1).HasValue());
            CHECK(fixture.probe.starts == 1);
            runtime.BeginShutdown();
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
        }

        class CommitProbe final : public IAiCookedDecisionIntentSink {
        public:
            Result<bool> Commit(const AiDecisionSliceContext &, AiWorkBudget &) noexcept override {
                ++commits;
                return Result<bool>::Success(true);
            }

            void Discard(AgentHandle) noexcept override {
                ++discards;
            }

            std::size_t commits{};
            std::size_t discards{};
        };

        TEST_CASE("Scene decision revocation retires work without another provider event",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            std::array sources{TaskTree(11)};
            sources.front().executionTask = MakeIdentity<TaskId>(9000);
            const auto plan = fixture.Compile(sources, 11);
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto publication = TestSupport::Publish(runtime, scene);
            const auto intents = std::make_shared<CommitProbe>();
            REQUIRE(runtime
                        .ActivateCookedDecisionsAtSafePoint({.jobs = jobs,
                                                             .scheduler = {.mode = AiSchedulingMode::BestEffort},
                                                             .agent = {.intervalTicks = 0},
                                                             .intents = intents,
                                                             .timing = AiCookedDecisionTiming::BestEffortExternalFeedback})
                        .HasValue());
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            REQUIRE(scheduler->EvaluateAtDecision(1).Value().evaluated == 1);
            REQUIRE(fixture.probe.continuation.IsCurrent());

            SECTION("an already evaluated intent cannot dispatch after revocation") {
                fixture.admission->store(false);
                REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());
                CHECK(intents->commits == 0);
            }
            SECTION("a reactive task wakes for retirement without a queued result") {
                REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());
                REQUIRE(scheduler->EvaluateAtDecision(2).Value().evaluated == 0);
                REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());
                fixture.admission->store(false);
                REQUIRE(scheduler->EvaluateAtDecision(3).Value().evaluated == 1);
                REQUIRE(scheduler->CommitAtIntentDispatch(3).HasValue());
                CHECK(intents->commits == 1);
            }
            CHECK(intents->discards == 1);
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
            CHECK_FALSE(fixture.probe.continuation.IsCurrent());
        }

        TEST_CASE("Scene cooked tasks receive the admitted scheduler event after idle ticks",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            const auto watched = MakeIdentity<BlackboardKeyId>(1);
            const auto ignored = MakeIdentity<BlackboardKeyId>(2);
            const std::array keys{BlackboardKeyDescriptor{.key = watched, .defaultValue = BlackboardValue{BlackboardScalarValue{true}}},
                                  BlackboardKeyDescriptor{.key = ignored, .defaultValue = BlackboardValue{BlackboardScalarValue{true}}}};
            auto schema = BlackboardSchema::Capture({SchemaId(), 1, BlackboardUnknownValuePolicy::Reject, keys});
            REQUIRE(schema.HasValue());
            fixture.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
            auto node = TreeNode(2, DecisionControlKind::Task);
            node.requirements = {{.key = watched, .kind = BlackboardValueKind::Boolean, .source = Location()}};
            auto source = Tree(11, {TreeNode(1, DecisionControlKind::Root), node}, {Edge(1, 1, 2)});
            source.executionTask = MakeIdentity<TaskId>(9000);
            const auto plan = fixture.Compile(std::span{&source, 1}, 11);
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            REQUIRE(runtime
                        .ActivateCookedDecisionsAtSafePoint({.jobs = jobs,
                                                             .scheduler = {.mode = AiSchedulingMode::BestEffort},
                                                             .agent = {.intervalTicks = 0},
                                                             .intents = std::make_shared<CommitProbe>(),
                                                             .timing = AiCookedDecisionTiming::BestEffortExternalFeedback})
                        .HasValue());
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            const auto tick = [&](const std::uint64_t number) {
                const auto evaluated = scheduler->EvaluateAtDecision(number);
                REQUIRE(evaluated.HasValue());
                CHECK(evaluated.Value().failed == 0);
                REQUIRE(scheduler->CommitAtIntentDispatch(number).HasValue());
                return evaluated.Value().evaluated;
            };

            CHECK(tick(1) == 1);
            REQUIRE(fixture.probe.tasks.size() == 1);
            const auto task = fixture.probe.tasks.front();
            const auto capability = runtime.CapabilityAtSafePoint();
            const auto write = [&](BlackboardKeyId key, bool value) {
                auto opened = capability.BeginBlackboardWrite(agent);
                REQUIRE(opened.HasValue());
                auto batch = std::move(opened).Value();
                REQUIRE(batch.Stage({key, BlackboardValue{BlackboardScalarValue{value}}}).HasValue());
                const auto result = capability.CommitBlackboardWrite(agent, std::move(batch));
                REQUIRE(result.HasValue());
                return result.Value().changedKeyCount;
            };
            CHECK(write(ignored, false) == 1);
            CHECK(write(watched, true) == 0);
            CHECK(tick(2) == 0);
            CHECK(fixture.probe.resumes == 0);

            CHECK(write(watched, false) == 1);
            CHECK(write(watched, true) == 1);
            CHECK(tick(3) == 1);
            CHECK(fixture.probe.resumes == 1);
            CHECK(fixture.probe.lastResumeTask == task);
            CHECK(fixture.probe.lastResumeReason == AiTaskResumeReason::Event);
            CHECK(tick(4) == 0);
            const auto continuation = fixture.probe.continuation;
            REQUIRE(continuation.IsCurrent());
            auto publicationResult = AiTaskPublicationDisposition::Stale;
            {
                const std::jthread notifier{[&] {
                    publicationResult = continuation.NotifyEvent();
                }};
            }
            CHECK(publicationResult == AiTaskPublicationDisposition::Accepted);
            REQUIRE(continuation.NotifyEvent() == AiTaskPublicationDisposition::Accepted);
            CHECK(tick(5) == 1);
            CHECK(fixture.probe.resumes == 2);
            CHECK(fixture.probe.lastResumeReason == AiTaskResumeReason::Event);
            CHECK(tick(6) == 0);

            AiTaskTerminalResult terminal{.state = AiTaskState::Succeeded};
            REQUIRE(continuation.PublishTerminal(terminal).Value() == AiTaskPublicationDisposition::Accepted);
            CHECK(tick(7) == 1);
            CHECK(fixture.probe.resumes == 2);
            CHECK(fixture.probe.cleanups == 1);
            CHECK(fixture.probe.lastTerminal == AiTaskState::Succeeded);
            CHECK(continuation.NotifyEvent() == AiTaskPublicationDisposition::Stale);
            CHECK(tick(8) == 0);

            Tests::AllocationProbe::Measurement allocations;
            bool idle = true;
            {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                for (std::uint64_t number = 9; number <= 64; ++number) {
                    const auto evaluated = scheduler->EvaluateAtDecision(number);
                    const auto committed = scheduler->CommitAtIntentDispatch(number);
                    idle = idle && evaluated.HasValue() && evaluated.Value().evaluated == 0 && evaluated.Value().workUnits == 0 &&
                           committed.HasValue() && committed.Value().commands == 0;
                }
                allocations = measurement.Snapshot();
            }
            CHECK(idle);
            CHECK(allocations.requests == 0);
            runtime.BeginShutdown();
        }

        TEST_CASE("A cooked task wake during evaluation can resume only on a later tick",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            auto source = Tree(11, {TreeNode(1, DecisionControlKind::Root), TreeNode(2, DecisionControlKind::Task)}, {Edge(1, 1, 2)});
            source.executionTask = MakeIdentity<TaskId>(9000);
            const auto plan = fixture.Compile(std::span{&source, 1}, 11);
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto publication = TestSupport::Publish(runtime, scene);
            REQUIRE(runtime
                        .ActivateCookedDecisionsAtSafePoint({.jobs = jobs,
                                                             .scheduler = {.mode = AiSchedulingMode::BestEffort},
                                                             .agent = {.intervalTicks = 0},
                                                             .timing = AiCookedDecisionTiming::BestEffortReactive})
                        .HasValue());
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            AiTaskPublicationDisposition feedback = AiTaskPublicationDisposition::Stale;
            bool reentrantRejected{};
            fixture.probe.onStart = [&] {
                feedback = fixture.probe.continuation.NotifyEvent();
                reentrantRejected = scheduler->EvaluateAtDecision(1).HasError();
            };
            REQUIRE(scheduler->EvaluateAtDecision(1).Value().evaluated == 1);
            CHECK(feedback == AiTaskPublicationDisposition::Accepted);
            CHECK(reentrantRejected);
            CHECK(scheduler->EvaluateAtDecision(1).Value().evaluated == 1);
            CHECK(fixture.probe.resumes == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());

            REQUIRE(scheduler->EvaluateAtDecision(2).Value().evaluated == 1);
            CHECK(fixture.probe.resumes == 1);
            CHECK(fixture.probe.lastResumeReason == AiTaskResumeReason::Event);
            REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());
            CHECK(scheduler->EvaluateAtDecision(3).Value().evaluated == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(3).HasValue());
            runtime.BeginShutdown();
        }

        TEST_CASE("Event-only cooked graphs wake for active service and timeout deadlines",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            auto service = Registration(fixture.probe, 502);
            service.contract.role = DecisionNodeRole::Service;
            const std::array registrations{Registration(fixture.probe), service};
            const auto generation = DecisionNodeGeneration::Capture(registrations, fixture.admission);
            REQUIRE(generation.HasValue());
            fixture.providers = generation.Value();

            auto timer = TreeNode(2, DecisionControlKind::TimeLimit);
            timer.kind = BehaviorTreeNodeKind::Decorator;
            timer.minimumChildren = timer.maximumChildren = 1;
            timer.control.durationTicks = 4;
            auto periodic = TreeNode(4, DecisionControlKind::Service);
            periodic.kind = BehaviorTreeNodeKind::Service;
            periodic.type = MakeIdentity<BehaviorTreeNodeTypeId>(502);
            periodic.control.intervalTicks = 2;
            auto attached = Edge(3, 2, 4);
            attached.kind = BehaviorTreeEdgeKind::ServiceAttachment;
            auto source = Tree(11, {TreeNode(1, DecisionControlKind::Root), timer, TreeNode(3, DecisionControlKind::Task), periodic},
                               {Edge(1, 1, 2), Edge(2, 2, 3), attached});
            source.executionTask = MakeIdentity<TaskId>(9000);
            const auto plan = fixture.Compile(std::span{&source, 1}, 11);
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto publication = TestSupport::Publish(runtime, scene);
            REQUIRE(runtime
                        .ActivateCookedDecisionsAtSafePoint({.jobs = jobs,
                                                             .scheduler = {.mode = AiSchedulingMode::BestEffort},
                                                             .agent = {.intervalTicks = 0},
                                                             .timing = AiCookedDecisionTiming::BestEffortReactive})
                        .HasValue());
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            const auto evaluate = [&](const std::uint64_t tick) {
                const auto decision = scheduler->EvaluateAtDecision(tick);
                REQUIRE(decision.HasValue());
                CHECK(decision.Value().failed == 0);
                REQUIRE(scheduler->CommitAtIntentDispatch(tick).HasValue());
                return decision.Value().evaluated;
            };
            CHECK(evaluate(1) == 1);
            CHECK(fixture.probe.services == 1);
            CHECK(evaluate(2) == 0);
            CHECK(evaluate(3) == 1);
            CHECK(fixture.probe.services == 2);
            CHECK(fixture.probe.lastResumeReason == AiTaskResumeReason::FixedTick);
            CHECK(evaluate(4) == 0);
            CHECK(evaluate(5) == 1);
            CHECK(fixture.probe.services == 2);
            REQUIRE(fixture.probe.abortReasons.size() == 1);
            CHECK(fixture.probe.abortReasons.front() == AiTaskCancellationReason::TimedOut);
            CHECK(evaluate(6) == 0);
            runtime.BeginShutdown();
        }

        struct WakeSenseControl final {
            bool sees{true};
            bool fails{};
            std::uint32_t evaluations{};
        };

        class WakeSense final : public IPerceptionSenseProvider {
        public:
            explicit WakeSense(WakeSenseControl &control) : control_(control) {}

            Result<std::size_t> Evaluate(const PerceptionSenseInput &input, std::span<PerceptionSenseSample> output,
                                         AiWorkBudget &budget) override {
                ++control_.evaluations;
                if (control_.fails)
                    return Result<std::size_t>::Failure(MakeError(AIErrors::PerceptionProviderFailed));
                for (std::size_t index = 0; index < input.candidates.size(); ++index) {
                    if (!budget.Consume())
                        return Result<std::size_t>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
                    output[index] = {static_cast<std::uint32_t>(index), control_.sees};
                }
                return Result<std::size_t>::Success(input.candidates.size());
            }

        private:
            WakeSenseControl &control_;
        };

        /** @brief Registers a deterministic provider whose native control can be faulted on the owner lane. */
        PerceptionSenseRegistration RegisterWakeSense(const SenseTypeId sense, const StimulusTypeId stimulus,
                                                      const PerceptionCapabilitySet capabilities, WakeSenseControl &control) {
            const PerceptionDescriptorOrigin origin{PerceptionDescriptorSourceKind::Native, MakeIdentity<PerceptionProviderId>(22), 1};
            return {{{sense, origin, "reactive sight", capabilities}, {stimulus, origin, "visible actor", 1, {}}, sizeof(WakeSense), 2, 2},
                    {&control, [](void *value) -> IPerceptionSenseProvider * {
                return new WakeSense{*static_cast<WakeSenseControl *>(value)};
            }, [](void *, IPerceptionSenseProvider *provider) noexcept {
                delete provider;
            }}};
        }

        /** @brief Captures both listener routes independently against the actual provider registrations. */
        PerceptionDescriptorRegistry WakeSenseRegistry(const std::array<PerceptionSenseRegistration, 2> &registrations,
                                                       const std::array<PerceptionListenerTypeId, 2> &ids,
                                                       const PerceptionCapabilitySet capabilities) {
            const std::array senses{registrations[0].contract.sense, registrations[1].contract.sense};
            const std::array stimuli{registrations[0].contract.stimulus, registrations[1].contract.stimulus};
            std::array<PerceptionListenerDescriptor, 2> listeners{};
            for (std::size_t index = 0; index < listeners.size(); ++index) {
                listeners[index].identity = ids[index];
                listeners[index].origin = senses[index].origin;
                listeners[index].displayName = "reactive listener";
                listeners[index].sense = senses[index].identity;
                listeners[index].stimuli[0] = {stimuli[index].identity, 1, 1};
                listeners[index].stimulusCount = 1;
            }
            auto registry = PerceptionDescriptorRegistry::Capture({senses, stimuli, listeners}, capabilities);
            REQUIRE(registry.HasValue());
            return std::move(registry).Value();
        }

        TEST_CASE("Native perception wakes only declared cooked dependencies when decision facts change",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            const PerceptionBlackboardProjection keys{MakeIdentity<BlackboardKeyId>(80), MakeIdentity<BlackboardKeyId>(81)};
            const auto collection = [](const BlackboardKeyId key, const BlackboardValueKind kind) {
                return BlackboardKeyDescriptor{.key = key,
                                               .kind = kind,
                                               .cardinality = BlackboardValueCardinality::Collection,
                                               .maximumCollectionElements = 1,
                                               .defaultValue = BlackboardValue{BlackboardCollectionValue{.elementKind = kind}}};
            };
            const std::array descriptors{fixture.schema->Keys().front(), collection(keys.sources, BlackboardValueKind::EntityReference),
                                         collection(keys.positions, BlackboardValueKind::WorldCoordinate)};
            auto schema = BlackboardSchema::Capture({SchemaId(), 1, BlackboardUnknownValuePolicy::Reject, descriptors});
            REQUIRE(schema.HasValue());
            fixture.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());

            auto source = TaskTree(11);
            source.executionTask = MakeIdentity<TaskId>(9000);
            const auto plan = fixture.Compile(std::span{&source, 1}, 11);
            auto authored = SceneFixture(fixture, *plan);
            authored.descriptor.cookedPlan = plan;
            WakeSenseControl primary;
            WakeSenseControl secondary;
            const PerceptionSenseTestSupport::InputFixture native;
            const auto listener = native.binding.listenerType;
            const auto otherListener = MakeIdentity<PerceptionListenerTypeId>(41);
            authored.agent.controller->requiredCapabilities =
                authored.agent.controller->requiredCapabilities.Union(AiCapabilitySet::Of(AiCapability::Perception));
            const auto sense = MakeIdentity<SenseTypeId>(20);
            const auto otherSense = MakeIdentity<SenseTypeId>(21);
            const std::array registrations{RegisterWakeSense(sense, MakeIdentity<StimulusTypeId>(21), native.capabilities, primary),
                                           RegisterWakeSense(otherSense, MakeIdentity<StimulusTypeId>(22), native.capabilities, secondary)};
            auto generation = PerceptionSenseGeneration::Capture(registrations, std::make_shared<std::atomic_bool>(true));
            REQUIRE(generation.HasValue());
            authored.descriptor.perceptionEndpoints = std::make_shared<const AiControllerPerceptionEndpoints>(
                AiControllerPerceptionEndpoints{.registry = std::make_shared<const PerceptionDescriptorRegistry>(
                                                    WakeSenseRegistry(registrations, {listener, otherListener}, native.capabilities)),
                                                .listeners = {listener, otherListener},
                                                .sources = {sense, otherSense},
                                                .decisionWatches = {{Node(2), listener}},
                                                .blackboardProjection = keys});

            auto malformed = authored.descriptor;
            auto wrongKeys = std::make_shared<AiControllerPerceptionEndpoints>(*malformed.perceptionEndpoints);
            wrongKeys->blackboardProjection = {keys.sources, descriptors.front().key};
            malformed.perceptionEndpoints = std::move(wrongKeys);
            TestSupport::ExpectError(ValidateAiControllerDescriptor(malformed), AIErrors::BlackboardValueTypeMismatch);

            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
            for (std::uint64_t id = 1; id <= 2; ++id) {
                auto agent = authored.agent.agent;
                agent.agent = MakeIdentity<AgentId>(id);
                builder.Add(
                    {.object = Runtime::SceneObjectId{id}, .components = {.aiAgent = agent, .aiController = authored.agent.controller}});
            }
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto scene = Runtime::RuntimeScene::Create(definition.Value(), authored.binding.scene);
            REQUIRE(scene.HasValue());
            const auto view = scene.Value()->View();
            PerceptionSpatialBroadphase spatial;
            AiScenePerceptionActivationInput sensing{spatial,
                                                     {},
                                                     [](const Runtime::RuntimeSceneView &candidate) {
                std::vector<AiScenePerceptionPosition> positions;
                for (std::uint64_t id = 1; id <= 2; ++id)
                    positions.push_back({*candidate.Find(Runtime::SceneObjectId{id}),
                                         Math::WorldCoordinate64::FromMillimeters(static_cast<std::int64_t>(id) * 1000, 0, 0)});
                return Result<std::vector<AiScenePerceptionPosition>>::Success(std::move(positions));
            },
                                                     AiScenePerceptionExecution{.generation = generation.Value(),
                                                                                .capabilities = native.capabilities,
                                                                                .radiusMillimeters = 5000,
                                                                                .visibleLayers = 1,
                                                                                .workUnitsPerTick = 100}};
            enum class BudgetScenario { Full, Rotate, ExpireWhileWaiting };
            auto budgetScenario = BudgetScenario::Full;
            SECTION("two agents with two listeners each share one listener allowance") {
                budgetScenario = BudgetScenario::Rotate;
            }
            SECTION("a watched observation expires on the tick when another agent uses the sensing budget") {
                budgetScenario = BudgetScenario::ExpireWhileWaiting;
                sensing.execution->memory.memoryDurationSeconds = 0.001;
            }
            if (budgetScenario != BudgetScenario::Full) {
                sensing.execution->scheduling = AiScenePerceptionScheduling::RotateUnderBudget;
                sensing.execution->workUnitsPerTick =
                    registrations.front().contract.workUnits + PerceptionSenseGeneration::ValidationWorkUnits + 1;
            }

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            AiSceneActivationParticipant participant{runtime, std::span{&authored.descriptor, 1}, sensing};
            auto publication = participant.Prepare(definition.Value(), view);
            REQUIRE(publication.HasValue());
            publication.Value()->Publish();
            TestSupport::ExpectError(runtime.ActivateCookedDecisionsAtSafePoint({jobs, {}, {}}), AIErrors::SchedulerPolicyInvalid);
            REQUIRE(fixture.probe.factories == 0);
            REQUIRE(runtime
                        .ActivateCookedDecisionsAtSafePoint({.jobs = jobs,
                                                             .scheduler = {.mode = AiSchedulingMode::BestEffort},
                                                             .timing = AiCookedDecisionTiming::BestEffortReactive})
                        .HasValue());
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            const auto capability = runtime.CapabilityAtSafePoint();
            const auto agent = capability.ResolveAgent(MakeIdentity<AgentId>(1)).Value();
            const auto ignored = capability.ResolveAgent(MakeIdentity<AgentId>(2)).Value();
            REQUIRE(runtime.SetCookedPerceptionWatchesAtSafePoint(ignored, {}).HasValue());
            const std::array invalid{DecisionWakePerceptionDependency{Node(8), listener}};
            CHECK(runtime.SetCookedPerceptionWatchesAtSafePoint(agent, invalid).HasError());
            const auto evaluate = [&](const std::uint64_t tick) {
                const auto positions = sensing.positions(view);
                REQUIRE(positions.HasValue());
                REQUIRE(runtime.PublishPerceptionAtSafePoint(view, tick + 1, positions.Value(), spatial).HasValue());
                REQUIRE(runtime
                            .PollPerceptionAtSensePoll(view, spatial, tick,
                                                       Duration::FromNanoseconds(sensing.execution->memory.fixedStep.count()))
                            .HasValue());
                REQUIRE(runtime.PublishPerceptionBlackboardsAtBlackboardSync(view, spatial, tick).HasValue());
                REQUIRE(runtime.PublishPerceptionBlackboardsAtBlackboardSync(view, spatial, tick).HasValue());
                const auto decision = scheduler->EvaluateAtDecision(tick);
                REQUIRE(decision.HasValue());
                CHECK(decision.Value().failed == 0);
                REQUIRE(scheduler->CommitAtIntentDispatch(tick).HasValue());
                return decision.Value().evaluated;
            };
            if (budgetScenario == BudgetScenario::ExpireWhileWaiting) {
                CHECK(evaluate(1) == 2);
                CHECK(evaluate(2) == 1);
                CHECK(fixture.probe.resumes == 1);
                CHECK(fixture.probe.lastResumeReason == AiTaskResumeReason::Event);
                CHECK(primary.evaluations == 2);
                CHECK(secondary.evaluations == 0);
                return;
            }
            if (budgetScenario == BudgetScenario::Rotate) {
                const auto first = *view.Find(Runtime::SceneObjectId{1});
                const auto second = *view.Find(Runtime::SceneObjectId{2});
                for (std::uint64_t tick = 1; tick <= 4; ++tick) {
                    (void)evaluate(tick);
                    CHECK(primary.evaluations == std::min<std::uint64_t>(tick, 2));
                    CHECK(secondary.evaluations == (tick > 2 ? tick - 2 : 0));
                    const auto served = tick % 2 == 1 ? first : second;
                    const auto memory = runtime.ReadPerceptionAtSafePoint(view, served);
                    REQUIRE(memory.HasValue());
                    REQUIRE(memory.Value().count == (tick <= 2 ? 1 : 2));
                    const auto entries = std::span{memory.Value().entries}.first(memory.Value().count);
                    CHECK(std::ranges::count(entries, tick, &PerceivedStimulus::lastSensedTick) == 1);
                }
                return;
            }
            CHECK(evaluate(1) == 2);
            const auto oldBoard = runtime.CaptureDecisionSnapshotAtSafePoint(agent);
            REQUIRE(oldBoard.HasValue());
            const auto oldSources = oldBoard.Value().Read(keys.sources);
            REQUIRE(oldSources.HasValue());
            REQUIRE(oldSources.Value());
            CHECK(std::get<BlackboardCollectionValue>(*oldSources.Value()).count == 1);
            const auto oldPositions = oldBoard.Value().Read(keys.positions);
            REQUIRE(oldPositions.HasValue());
            REQUIRE(oldPositions.Value());
            const auto &positions = std::get<BlackboardCollectionValue>(*oldPositions.Value());
            REQUIRE(positions.count == 1);
            CHECK(std::get<Math::WorldCoordinate64>(positions.elements[0]) == Math::WorldCoordinate64::FromMillimeters(2000, 0, 0));
            const auto inspection = runtime.CaptureAgentInspectionAtSafePoint(view, agent, {});
            REQUIRE(inspection.HasValue());
            CHECK(inspection.Value().perceptionProjectionDrops == 1);
            CHECK(inspection.Value().stimuli.size() == 2);
            CHECK(evaluate(2) == 0);
            CHECK(oldBoard.Value().Read(keys.sources).HasError());
            const auto freshBoard = runtime.CaptureDecisionSnapshotAtSafePoint(agent);
            REQUIRE(freshBoard.HasValue());
            CHECK(freshBoard.Value().CapturedRevision() == oldBoard.Value().CapturedRevision());
            CHECK(freshBoard.Value().Read(keys.sources).HasValue());
            primary.sees = false;
            CHECK(evaluate(3) == 1);
            CHECK(fixture.probe.resumes == 1);
            CHECK(fixture.probe.lastResumeReason == AiTaskResumeReason::Event);
            CHECK(evaluate(4) == 0);
            SECTION("removing the watch leaves later observations idle") {
                REQUIRE(runtime.SetCookedPerceptionWatchesAtSafePoint(agent, {}).HasValue());
                primary.sees = true;
                CHECK(evaluate(5) == 0);
            }
            SECTION("typed provider failure withdraws only its lane and wakes the watching decision") {
                const auto recipient = *view.Find(Runtime::SceneObjectId{1});
                primary.sees = true;
                REQUIRE(evaluate(5) == 1);
                const auto before = runtime.ReadPerceptionAtSafePoint(view, recipient);
                REQUIRE(before.HasValue());
                REQUIRE(before.Value().count == 2);

                primary.fails = true;
                CHECK(evaluate(6) == 1);
                const auto retained = runtime.ReadPerceptionAtSafePoint(view, recipient);
                REQUIRE(retained.HasValue());
                REQUIRE(retained.Value().count == 1);
                CHECK(retained.Value().entries[0].key.listener == otherListener);
                const auto inspection = runtime.CaptureAgentInspectionAtSafePoint(view, agent, {});
                REQUIRE(inspection.HasValue());
                REQUIRE(inspection.Value().senseProviders.size() == 2);
                CHECK_FALSE(inspection.Value().senseProviders[0].available);
                CHECK(inspection.Value().senseProviders[1].available);

                const auto failedPolls = primary.evaluations;
                const auto activePolls = secondary.evaluations;
                CHECK(evaluate(7) == 0);
                CHECK(primary.evaluations == failedPolls);
                CHECK(secondary.evaluations == activePolls + 2);
                secondary.fails = true;
                CHECK(evaluate(8) == 0);
                TestSupport::ExpectError(runtime.ReadPerceptionAtSafePoint(view, recipient), AIErrors::PerceptionProviderFailed);
                const auto cleared = capability.ReadBlackboard(agent, keys.sources);
                REQUIRE(cleared.HasValue());
                REQUIRE(cleared.Value());
                CHECK(std::get<BlackboardCollectionValue>(*cleared.Value()).count == 0);
            }
            SECTION("the structural publication of a retired source wakes the surviving watcher") {
                auto structural = runtime.MakeStructuralParticipant(std::span{&authored.descriptor, 1});
                const std::array removed{*view.Find(Runtime::SceneObjectId{2})};
                {
                    const auto staged = structural->Prepare(view, {}, removed);
                    REQUIRE(staged.HasValue());
                    REQUIRE(staged.Value()->ValidatePublication().HasValue());
                }
                CHECK(evaluate(5) == 0);

                auto prepared = structural->Prepare(view, {}, removed);
                REQUIRE(prepared.HasValue());
                REQUIRE(prepared.Value()->ValidatePublication().HasValue());
                prepared.Value()->Publish();
                REQUIRE(prepared.Value()->AfterPublication().HasValue());

                const auto decision = scheduler->EvaluateAtDecision(6);
                REQUIRE(decision.HasValue());
                CHECK(decision.Value().failed == 0);
                CHECK(decision.Value().evaluated == 1);
                CHECK(fixture.probe.resumes == 2);
                CHECK(fixture.probe.lastResumeReason == AiTaskResumeReason::Event);
                REQUIRE(scheduler->CommitAtIntentDispatch(6).HasValue());
            }
            runtime.BeginShutdown();
        }

        TEST_CASE("Scene decision replacement fences the old image and preserves the Blackboard across a clean restart",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture old;
            Fixture next;
            Fixture failed;
            old.probe.succeedTick = 100;
            next.probe.succeedTick = 100;
            next.schema = old.schema;
            failed.schema = old.schema;
            auto oldSource = TaskTree(11);
            oldSource.executionTask = MakeIdentity<TaskId>(9000);
            const auto previous = old.Compile(std::span{&oldSource, 1}, 11);
            auto newSource = Tree(11, {TreeNode(1, DecisionControlKind::Root), TreeNode(3, DecisionControlKind::Task)}, {Edge(1, 1, 3)});
            newSource.executionTask = oldSource.executionTask;

            struct ReloadFactory final {
                Probe &probe;
                std::function<void()> beforeCreate;
            } replacementFactory{next.probe, {}};

            auto registration = Registration(next.probe);
            registration.factory.userData = &replacementFactory;
            registration.factory.create = [](void *owner, std::span<const BehaviorTreeProperty> properties) -> IBehaviorTreeExecutor * {
                auto &factory = *static_cast<ReloadFactory *>(owner);
                if (factory.beforeCreate)
                    factory.beforeCreate();
                return Create(&factory.probe, properties);
            };
            auto nextGeneration = DecisionNodeGeneration::Capture({&registration, 1}, next.admission);
            REQUIRE(nextGeneration.HasValue());
            next.providers = std::move(nextGeneration).Value();
            const auto replacement = next.Compile(std::span{&newSource, 1}, 11);

            auto failing = Registration(failed.probe);
            failing.factory.create = [](void *, std::span<const BehaviorTreeProperty>) -> IBehaviorTreeExecutor * {
                return nullptr;
            };
            auto generation = DecisionNodeGeneration::Capture({&failing, 1}, failed.admission);
            REQUIRE(generation.HasValue());
            failed.providers = std::move(generation).Value();
            const auto rejected = failed.Compile(std::span{&newSource, 1}, 11);

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {}}).Value();
            auto scene = SceneFixture(old, *previous);
            scene.descriptor.cookedPlan = previous;
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            const auto capability = runtime.CapabilityAtSafePoint();
            auto write = capability.BeginBlackboardWrite(agent);
            REQUIRE(write.HasValue());
            const auto key = MakeIdentity<BlackboardKeyId>(1);
            auto batch = std::move(write).Value();
            REQUIRE(batch.Stage({key, BlackboardScalarValue{false}}).HasValue());
            REQUIRE(capability.CommitBlackboardWrite(agent, std::move(batch)).HasValue());
            const auto retained = runtime.CaptureDecisionSnapshotAtSafePoint(agent);
            REQUIRE(retained.HasValue());

            REQUIRE(scheduler->EvaluateAtDecision(1).HasValue());
            CHECK(old.probe.starts == 1);
            TestSupport::ExpectError(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement),
                                     AIErrors::SchedulerPhaseInvalid);
            CHECK(next.probe.factories == 0);
            CHECK(old.probe.aborts == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());
            TestSupport::ExpectError(runtime.ReplaceCookedDecisionAtSafePoint(agent, rejected, replacement),
                                     AIErrors::DecisionAssetActivationInvalid);
            TestSupport::ExpectError(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, rejected), AIErrors::RuntimeUnavailable);
            CHECK(old.probe.aborts == 0);
            REQUIRE(scheduler->EvaluateAtDecision(2).HasValue());
            CHECK(old.probe.resumes == 1);
            CHECK(old.probe.lastResumeReason == AiTaskResumeReason::FixedTick);
            REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());

            std::optional<Result<void>> nestedReplacement;
            replacementFactory.beforeCreate = [&nestedReplacement, &runtime, agent, &previous, &replacement] {
                if (!nestedReplacement.has_value()) {
                    nestedReplacement = Result<void>::Success();
                    nestedReplacement = runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement);
                }
            };
            bool evaluationDuringAbortRejected{};
            old.probe.onAbort = [&evaluationDuringAbortRejected, scheduler] {
                evaluationDuringAbortRejected = scheduler->EvaluateAtDecision(3).HasError();
            };
            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement).HasValue());
            REQUIRE(nestedReplacement.has_value());
            TestSupport::ExpectError(*nestedReplacement, AIErrors::SchedulerPhaseInvalid);
            CHECK(next.probe.factories == 1);
            CHECK(evaluationDuringAbortRejected);
            REQUIRE(old.probe.abortReasons.size() == 1);
            CHECK(old.probe.abortReasons.front() == AiTaskCancellationReason::PlanReplaced);
            CHECK(old.probe.cleanups == 1);
            CHECK(next.probe.starts == 0);
            TestSupport::ExpectError(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, rejected),
                                     AIErrors::DecisionAssetActivationInvalid);
            TestSupport::ExpectError(scheduler->EvaluateAtDecision(2), AIErrors::SchedulerPhaseInvalid);

            const auto current = runtime.CaptureDecisionSnapshotAtSafePoint(agent);
            REQUIRE(current.HasValue());
            CHECK(current.Value().Binding() == retained.Value().Binding());
            REQUIRE(current.Value().Revision().HasValue());
            REQUIRE(retained.Value().Revision().HasValue());
            CHECK(current.Value().Revision().Value() == retained.Value().Revision().Value());
            const BlackboardValue preserved{BlackboardScalarValue{false}};
            CHECK(current.Value().Read(key).Value() == std::optional{preserved});
            CHECK(retained.Value().Read(key).Value() == std::optional{preserved});
            REQUIRE(scheduler->EvaluateAtDecision(3).HasValue());
            REQUIRE(scheduler->CommitAtIntentDispatch(3).HasValue());
            REQUIRE(old.probe.tasks.size() == 1);
            REQUIRE(next.probe.tasks.size() == 1);
            CHECK(next.probe.tasks.front().slot.index == old.probe.tasks.front().slot.index + previous->Storage().taskSlots + 1);
            CHECK(old.probe.aborts == 1);
            CHECK(old.probe.cleanups == 1);
        }

        TEST_CASE("Scene decision reload publishes a migrated Blackboard and retires the old generation at one cutoff",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture old;
            Fixture next;
            old.probe.succeedTick = 100;
            const auto oldKey = MakeIdentity<BlackboardKeyId>(1);
            const auto newKey = MakeIdentity<BlackboardKeyId>(2);
            const std::array keys{BlackboardKeyDescriptor{.key = newKey, .defaultValue = BlackboardValue{BlackboardScalarValue{true}}}};
            auto schema = BlackboardSchema::Capture({old.schema->Identity(), 2, BlackboardUnknownValuePolicy::Reject, keys});
            REQUIRE(schema.HasValue());
            next.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
            auto source = TaskTree(11);
            source.executionTask = MakeIdentity<TaskId>(9000);
            auto newNode = TreeNode(2, DecisionControlKind::Task);
            newNode.requirements = {{.key = newKey, .kind = BlackboardValueKind::Boolean, .source = Location()}};
            auto newSource = Tree(11, {TreeNode(1, DecisionControlKind::Root), newNode}, {Edge(1, 1, 2)});
            newSource.blackboardVersion = {2, 2};
            newSource.executionTask = source.executionTask;
            const auto previous = old.Compile(std::span{&source, 1}, 11);
            const auto replacement = next.Compile(std::span{&newSource, 1}, 11);
            const std::array mapping{BlackboardKeyMigration{oldKey, newKey}};
            const BlackboardSchemaMigration valid{old.schema, next.schema->Identity(), next.schema->Version(), mapping};
            const BlackboardSchemaMigration missingMapping{old.schema, next.schema->Identity(), next.schema->Version(), {}};

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {}}).Value();
            auto scene = SceneFixture(old, *previous);
            scene.descriptor.cookedPlan = previous;
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            const auto capability = runtime.CapabilityAtSafePoint();
            auto write = std::move(capability.BeginBlackboardWrite(agent)).Value();
            const BlackboardValue preserved{BlackboardScalarValue{false}};
            REQUIRE(write.Stage({oldKey, preserved}).HasValue());
            REQUIRE(capability.CommitBlackboardWrite(agent, std::move(write)).HasValue());
            const auto oldSnapshot = runtime.CaptureDecisionSnapshotAtSafePoint(agent).Value();
            auto oldBatch = std::move(capability.BeginBlackboardWrite(agent)).Value();
            REQUIRE(oldBatch.Stage({oldKey, BlackboardScalarValue{true}}).HasValue());
            const auto reservation = runtime.ReserveDecisionTaskSlotsAtSafePoint(agent, 1);
            REQUIRE(reservation.HasValue());
            REQUIRE(scheduler->EvaluateAtDecision(1).HasValue());
            const auto continuation = old.probe.continuation;
            REQUIRE(continuation.IsCurrent());
            TestSupport::ExpectError(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement, &valid),
                                     AIErrors::SchedulerPhaseInvalid);
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());

            TestSupport::ExpectError(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement),
                                     AIErrors::DecisionAssetSchemaIncompatible);
            CHECK(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement, &missingMapping).HasError());
            CHECK(reservation.Value().CanAdmit());
            CHECK(oldSnapshot.Read(oldKey).Value() == std::optional{preserved});
            CHECK(continuation.IsCurrent());
            CHECK(next.probe.factories == 0);
            REQUIRE(scheduler->EvaluateAtDecision(2).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());
            CHECK(old.probe.resumes == 1);

            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement, &valid).HasValue());
            CHECK_FALSE(reservation.Value().CanAdmit());
            CHECK_FALSE(continuation.IsCurrent());
            CHECK(oldSnapshot.Read(oldKey).HasError());
            CHECK(capability.CommitBlackboardWrite(agent, std::move(oldBatch)).HasError());
            REQUIRE(old.probe.abortReasons.size() == 1);
            CHECK(old.probe.abortReasons.front() == AiTaskCancellationReason::PlanReplaced);
            CHECK(old.probe.cleanups == 1);
            CHECK(next.probe.starts == 0);

            const auto current = runtime.CaptureDecisionSnapshotAtSafePoint(agent).Value();
            CHECK(current.Binding().agent == agent);
            CHECK(current.Binding().schemaVersion == 2);
            CHECK(current.Binding().schemaGeneration > oldSnapshot.Binding().schemaGeneration);
            CHECK(current.Read(newKey).Value() == std::optional{preserved});
            REQUIRE(scheduler->EvaluateAtDecision(3).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(3).HasValue());
            CHECK(next.probe.starts == 1);
        }

        TEST_CASE("A compatible Scene recook retains the exact running task and detached completion",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            auto root = TreeNode(1, DecisionControlKind::Root);
            auto task = TreeNode(2, DecisionControlKind::Task);
            task.properties.emplace_back(MakeIdentity<BehaviorTreePropertyId>(44), "provider flag", BehaviorTreePropertyKind::Boolean,
                                         false);
            auto registration = Registration(fixture.probe);
            registration.contract.properties = task.properties;
            auto generation = DecisionNodeGeneration::Capture({&registration, 1}, fixture.admission);
            REQUIRE(generation.HasValue());
            fixture.providers = std::move(generation).Value();
            const auto compile = [&](std::string title, bool enabled = false) {
                root.displayName = std::move(title);
                task.properties.front().value = enabled;
                auto source = Tree(11, {root, task}, {Edge(1, 1, 2)});
                source.executionTask = MakeIdentity<TaskId>(9000);
                return fixture.Compile(std::span{&source, 1}, 11);
            };
            const auto original = compile("Original root caption");
            const auto edited = compile("First edited caption");
            const auto editedAgain = compile("Second edited caption");
            REQUIRE(original->Digest() != edited->Digest());
            REQUIRE(edited->Digest() != editedAgain->Digest());

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {}}).Value();
            auto scene = SceneFixture(fixture, *original);
            scene.descriptor.cookedPlan = original;
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            REQUIRE(scheduler->EvaluateAtDecision(1).HasValue());
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());
            REQUIRE(fixture.probe.tasks.size() == 1);
            const auto taskHandle = fixture.probe.tasks.front();
            const auto continuation = fixture.probe.continuation;
            REQUIRE(continuation.IsCurrent());

            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, original, edited).HasValue());
            CHECK(continuation.IsCurrent());
            CHECK(fixture.probe.factories == 1);
            CHECK(fixture.probe.aborts == 0);
            CHECK(fixture.probe.cleanups == 0);
            TestSupport::ExpectError(scheduler->EvaluateAtDecision(1), AIErrors::SchedulerPhaseInvalid);
            TestSupport::ExpectError(runtime.ReplaceCookedDecisionAtSafePoint(agent, original, editedAgain),
                                     AIErrors::DecisionAssetActivationInvalid);
            REQUIRE(scheduler->EvaluateAtDecision(2).HasValue());
            REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());
            CHECK(fixture.probe.resumes == 1);
            CHECK(fixture.probe.lastResumeTask == taskHandle);

            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, edited, editedAgain).HasValue());
            REQUIRE(continuation.IsCurrent());
            SECTION("original mailbox publishes completion to the still-running task") {
                AiTaskTerminalResult completed{.state = AiTaskState::Succeeded};
                REQUIRE(continuation.PublishTerminal(completed).Value() == AiTaskPublicationDisposition::Accepted);
                REQUIRE(scheduler->EvaluateAtDecision(3).HasValue());
                REQUIRE(scheduler->CommitAtIntentDispatch(3).HasValue());
                CHECK_FALSE(continuation.IsCurrent());
                CHECK(fixture.probe.factories == 1);
                CHECK(fixture.probe.starts == 1);
                CHECK(fixture.probe.aborts == 0);
                CHECK(fixture.probe.cleanups == 1);
                const auto unused = runtime.ReserveDecisionTaskSlotsAtSafePoint(agent, 1);
                REQUIRE(unused.HasValue());
                CHECK(unused.Value().FirstSlot() == original->Storage().taskSlots + 1);
            }
            SECTION("changed authored properties require a new executor despite identical tree bindings") {
                const auto changed = compile("Second edited caption", true);
                using TreePlan = std::shared_ptr<const BehaviorTreeExecutionPlan>;
                const auto &existingTree = std::get<TreePlan>(editedAgain->Graphs().front().execution);
                const auto &changedTree = std::get<TreePlan>(changed->Graphs().front().execution);
                REQUIRE(existingTree->IsCompatible(*changedTree));
                REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, editedAgain, changed).HasValue());
                CHECK_FALSE(continuation.IsCurrent());
                CHECK(fixture.probe.factories == 2);
                CHECK(fixture.probe.aborts == 1);
                CHECK(fixture.probe.cleanups == 1);
                REQUIRE(scheduler->EvaluateAtDecision(3).HasValue());
                REQUIRE(fixture.probe.tasks.size() == 2);
                CHECK(fixture.probe.tasks.back() != taskHandle);
            }
        }

        TEST_CASE("Compatible Scene recook retains independent nested tasks and completions",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            auto root = ParallelCalls(11, 12, DecisionPlanKind::BehaviorTree);
            root.executionTask = MakeIdentity<TaskId>(9000);
            auto childRoot = TreeNode(1, DecisionControlKind::Root);
            const auto compile = [&fixture, &root, &childRoot] {
                const std::array sources{root, Tree(12, {childRoot, TreeNode(2, DecisionControlKind::Task)}, {Edge(1, 1, 2)})};
                return fixture.Compile(sources, 11);
            };
            const auto previous = compile();
            childRoot.displayName = "Renamed child root";
            const auto replacement = compile();
            REQUIRE(previous->Digest() != replacement->Digest());
            REQUIRE(previous->Storage().occurrences == 3);
            const auto allowance = previous->MaximumDecisionWorkUnits();
            REQUIRE(allowance.has_value());
            CHECK(allowance == replacement->MaximumDecisionWorkUnits());

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {.workUnitsPerTick = *allowance}}).Value();
            auto scene = SceneFixture(fixture, *previous);
            scene.descriptor.cookedPlan = previous;
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            REQUIRE(scheduler->EvaluateAtDecision(1).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());
            REQUIRE(fixture.probe.tasks.size() == 2);
            CHECK(fixture.probe.tasks.front() != fixture.probe.tasks.back());
            const auto completion = fixture.probe.continuation;
            REQUIRE(completion.IsCurrent());

            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement).HasValue());
            CHECK(fixture.probe.factories == 2);
            CHECK(fixture.probe.aborts == 0);
            REQUIRE(completion.IsCurrent());
            AiTaskTerminalResult completed{.state = AiTaskState::Succeeded};
            REQUIRE(completion.PublishTerminal(completed).Value() == AiTaskPublicationDisposition::Accepted);
            TestSupport::ExpectError(scheduler->EvaluateAtDecision(1), AIErrors::SchedulerPhaseInvalid);
            REQUIRE(scheduler->EvaluateAtDecision(2).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());
            CHECK(fixture.probe.starts == 2);
            CHECK(fixture.probe.resumes == 1);
            CHECK(fixture.probe.lastResumeTask == fixture.probe.tasks.front());
            CHECK(fixture.probe.cleanups == 1);
            CHECK_FALSE(completion.IsCurrent());
        }

        TEST_CASE("Mixed Scene recook retains equivalent state and restarts on a changed transition or Utility policy",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            const bool editTransition = GENERATE(false, true);
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            const auto key = MakeIdentity<BlackboardKeyId>(101);
            const std::array keys{BlackboardKeyDescriptor{.key = key,
                                                          .kind = BlackboardValueKind::Scalar,
                                                          .defaultValue = BlackboardValue{BlackboardScalarValue{0.5}}}};
            auto schema = BlackboardSchema::Capture({.identity = SchemaId(), .keys = keys});
            REQUIRE(schema.HasValue());
            fixture.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());

            auto root = TreeNode(1, DecisionControlKind::Root);
            auto call = TreeNode(2, DecisionControlKind::Subplan);
            call.call = DecisionSubplanCall{Graph(12), DecisionPlanKind::StateMachine, {1, 1}};
            auto machine = CallingMachine(12, 1, DecisionPlanKind::Utility);
            auto utility = UtilitySource();
            const auto compile = [&fixture, &root, &call, &machine, &utility] {
                auto tree = Tree(11, {root, call}, {Edge(1, 1, 2)});
                tree.executionTask = MakeIdentity<TaskId>(9000);
                const std::array sources{tree, machine, utility};
                return fixture.Compile(sources, 11);
            };
            const auto original = compile();
            root.displayName = "Revised root caption";
            const auto equivalent = compile();
            REQUIRE(original->Digest() != equivalent->Digest());
            if (editTransition)
                std::get<StateMachineAssetDescriptor>(machine.body).transitions.front().childOutcome = AiTaskState::Failed;
            else
                std::get<UtilityDecisionSource>(utility.body).options.front().option.priority = 0.5;
            const auto changed = compile();

            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            const auto units = std::max(original->MaximumDecisionWorkUnits().value(), changed->MaximumDecisionWorkUnits().value());
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {.workUnitsPerTick = units}}).Value();
            auto scene = SceneFixture(fixture, *original);
            scene.descriptor.cookedPlan = original;
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            REQUIRE(scheduler->EvaluateAtDecision(1).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());
            REQUIRE(fixture.probe.tasks.size() == 1);
            const auto task = fixture.probe.tasks.front();
            const auto completion = fixture.probe.continuation;

            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, original, equivalent).HasValue());
            CHECK(fixture.probe.factories == 1);
            CHECK(fixture.probe.aborts == 0);
            CHECK(completion.IsCurrent());
            REQUIRE(scheduler->EvaluateAtDecision(2).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());
            CHECK(fixture.probe.lastResumeTask == task);

            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, equivalent, changed).HasValue());
            CHECK_FALSE(completion.IsCurrent());
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
            CHECK(fixture.probe.starts == 1);
            REQUIRE(scheduler->EvaluateAtDecision(3).Value().failed == 0);
            REQUIRE(scheduler->CommitAtIntentDispatch(3).HasValue());
            REQUIRE(fixture.probe.tasks.size() == 2);
            CHECK(fixture.probe.tasks.back() != task);
        }

        TEST_CASE("Cooked Scene reports raw task callback errors without failing a successful Selector fallback",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            const auto providerError = MakeError(AIErrors::CapabilityUnavailable, "The registered task failed in its callback.");
            std::uint64_t failureTick = 1;
            bool advisory = true;
            SECTION("Start returns an error") {
                fixture.probe.startOutcome = Result<AiTaskState>::Failure(providerError);
            }
            SECTION("Resume returns an error") {
                fixture.probe.resumeOutcome = Result<AiTaskState>::Failure(providerError);
                failureTick = 2;
            }
            SECTION("The task deliberately returns Failed") {
                fixture.probe.startOutcome = Result<AiTaskState>::Success(AiTaskState::Failed);
                advisory = false;
            }

            const std::array registrations{Registration(fixture.probe), Registration(fixture.probe, 502, true)};
            auto generation = DecisionNodeGeneration::Capture(registrations, fixture.admission);
            REQUIRE(generation.HasValue());
            fixture.providers = std::move(generation).Value();
            auto selector = TreeNode(2, DecisionControlKind::Selector);
            selector.kind = BehaviorTreeNodeKind::Composite;
            selector.minimumChildren = 2;
            selector.maximumChildren = 2;
            selector.pins.back().cardinality = BehaviorTreePinCardinality::Multiple;
            auto fallback = TreeNode(4, DecisionControlKind::Task);
            fallback.type = MakeIdentity<BehaviorTreeNodeTypeId>(502);
            auto source = Tree(11, {TreeNode(1, DecisionControlKind::Root), selector, TreeNode(3, DecisionControlKind::Task), fallback},
                               {Edge(1, 1, 2), Edge(2, 2, 3), Edge(3, 2, 4, 1)});
            source.executionTask = MakeIdentity<TaskId>(9000);
            const auto plan = fixture.Compile(std::span{&source, 1}, 11);
            auto selected = SceneFixture(fixture, *plan);
            selected.descriptor.cookedPlan = plan;
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            builder.Add({.object = {1}, .components = {.aiAgent = selected.agent.agent, .aiController = selected.agent.controller}});
            const auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            const auto scene = Runtime::RuntimeScene::Create(definition.Value(), {8});
            REQUIRE(scene.HasValue());
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {}}).Value();
            const std::array descriptors{selected.descriptor};
            AiSceneActivationParticipant participant{runtime, descriptors};
            auto candidate = participant.Prepare(definition.Value(), scene.Value()->View());
            REQUIRE(candidate.HasValue());
            REQUIRE(candidate.Value()->ValidatePublication().HasValue());
            candidate.Value()->Publish();
            const auto snapshot = runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto &agent = snapshot.Value().Agents().front();
            const auto scheduler = runtime.TaskSchedulerAtSafePoint();
            REQUIRE(scheduler.HasValue());

            if (failureTick == 2) {
                REQUIRE(scheduler.Value()->EvaluateAtDecision(1).HasValue());
                REQUIRE(scheduler.Value()->CommitAtIntentDispatch(1).HasValue());
                CHECK_FALSE(runtime.ReadDecisionFailureAtSafePoint(scene.Value()->View()).Value());
                CHECK(fixture.probe.starts == 1);
            }
            const auto evaluated = scheduler.Value()->EvaluateAtDecision(failureTick);
            REQUIRE(evaluated.HasValue());
            CHECK(evaluated.Value().failed == 0);
            CHECK_FALSE(evaluated.Value().firstFailure);
            CHECK(evaluated.Value().firstAdvisoryFailure.has_value() == advisory);
            CHECK_FALSE(runtime.ReadDecisionFailureAtSafePoint(scene.Value()->View()).Value());
            REQUIRE(scheduler.Value()->CommitAtIntentDispatch(failureTick).HasValue());
            CHECK(fixture.probe.starts == 2);
            CHECK(fixture.probe.resumes == failureTick - 1);
            CHECK(fixture.probe.cleanups == 2);
            const auto diagnostic = runtime.ReadDecisionFailureAtSafePoint(scene.Value()->View());
            REQUIRE(diagnostic.HasValue());
            REQUIRE(diagnostic.Value().has_value() == advisory);
            if (advisory) {
                const auto &failure = *diagnostic.Value();
                CHECK(failure.tick == failureTick);
                CHECK(failure.origin.agent == agent.handle);
                CHECK(failure.origin.identity == agent.agent.agent);
                CHECK(failure.origin.phase == AiSchedulingFailurePhase::Evaluate);
                CHECK(failure.origin.advisory);
                CHECK(failure.origin.asset == Graph(11));
                CHECK(failure.origin.node == Node(3));
                CHECK(failure.entity == agent.owner);
                CHECK(failure.error.code.Value() == providerError.code.Value());
                CHECK(failure.error.message == providerError.message);
            }
            REQUIRE(scheduler.Value()->EvaluateAtDecision(failureTick + 1).HasValue());
            REQUIRE(scheduler.Value()->CommitAtIntentDispatch(failureTick + 1).HasValue());
            CHECK_FALSE(runtime.ReadDecisionFailureAtSafePoint(scene.Value()->View()).Value());
        }

        TEST_CASE("Restored manual AI receives its saved Blackboard before its cooked scheduler starts",
                  "[unit][ai][scene][scheduler][restore]") {
            Fixture fixture;
            auto source = TaskTree(11);
            source.executionTask = MakeIdentity<TaskId>(9000);
            const auto plan = fixture.Compile(std::span{&source, 1}, 11);
            auto selected = SceneFixture(fixture, *plan);
            selected.agent.agent.startupPolicy = AiStartupPolicy::Manual;
            selected.descriptor.cookedPlan = plan;
            auto blackboard = fixture.blackboard->CaptureCanonical();
            REQUIRE(blackboard.HasValue());
            auto values = std::move(blackboard).Value();
            values.entries.front().value = BlackboardValue{BlackboardScalarValue{false}};
            const AiCanonicalState saved{.agents = {{selected.agent.agent, AiCanonicalAgentState::Active,
                                                     AiBaseControllerCanonicalState{*selected.agent.controller, std::move(values)}}}};
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            builder.Add({.object = {1}, .components = {.aiAgent = selected.agent.agent, .aiController = selected.agent.controller}});
            const auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            const auto scene = Runtime::RuntimeScene::Create(definition.Value(), {8});
            REQUIRE(scene.HasValue());
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {}}).Value();
            auto candidate = runtime.PrepareRestoredScene(scene.Value()->View(), std::array{selected.descriptor}, saved);
            REQUIRE(candidate.HasValue());
            CHECK(fixture.probe.starts == 0);
            REQUIRE(candidate.Value()->ValidatePublication().HasValue());
            candidate.Value()->Publish();
            CHECK(runtime.CaptureCanonicalState(scene.Value()->View()).Value() == saved);
            const auto scheduler = runtime.TaskSchedulerAtSafePoint();
            REQUIRE(scheduler.HasValue());
            REQUIRE(scheduler.Value()->EvaluateAtDecision(1).HasValue());
            REQUIRE(scheduler.Value()->CommitAtIntentDispatch(1).HasValue());
            CHECK(fixture.probe.starts == 1);
        }

        /** @brief Uses a real admitted native query and observes the Scene scheduler's exact owner operation. */
        class PointQueryOwner final : public IDecisionQueryTaskBinder, public IDecisionQueryTaskHost {
        public:
            PointQueryOwner(const Runtime::RuntimeScene &scene, Runtime::EntityRef entity, EnvironmentQueryPlan plan,
                            QuerySchemaRegistry schema)
                : scene_(scene), entity_(entity), plan_(std::move(plan)), schema_(std::move(schema)) {}

            Result<std::unique_ptr<IBehaviorTreeExecutor>> BindQueryTask(const BehaviorTreeExecutionPlan &tree,
                                                                         const DecisionNodeConfiguration &configuration) override {
                using Executor = Result<std::unique_ptr<IBehaviorTreeExecutor>>;
                auto bound = DecisionQueryTask::BindAuthored(configuration, plan_, schema_, tree, *this);
                if (bound.HasError())
                    return Executor::Failure(bound.ErrorValue());
                ++bindings;
                task = bound.Value().get();
                return Executor::Success(std::move(bound).Value());
            }

            Result<std::unique_ptr<QueryExecution>> Admit(const DecisionQueryTaskDescriptor &, const AiTaskOperationContext &operation,
                                                          std::uint64_t tick, std::uint64_t deadline) override {
                using Execution = Result<std::unique_ptr<QueryExecution>>;
                const auto providers = QueryContextProviderRegistry::Capture({}, schema_);
                if (providers.HasError())
                    return Execution::Failure(providers.ErrorValue());
                QueryContextCapture capture;
                const auto current = scene_.View();
                const auto contexts =
                    capture.Capture(plan_, schema_, providers.Value(), current, {.querier = entity_, .executionRevision = tick});
                if (contexts.HasError())
                    return Execution::Failure(contexts.ErrorValue());
                auto admitted =
                    AdmitBuiltinPointQuery(plan_, schema_, current, {operation.agent, contexts.Value(), operation.ownerCancellation},
                                           {.submittedTick = tick, .deadlineTick = deadline, .maximumCandidates = 9, .bestCount = 1});
                if (admitted.HasValue()) {
                    ++admissions;
                    observed = operation;
                }
                return admitted;
            }

            std::size_t bindings{};
            std::size_t admissions{};
            std::optional<AiTaskOperationContext> observed;
            DecisionQueryTask *task{};

        private:
            const Runtime::RuntimeScene &scene_;
            Runtime::EntityRef entity_;
            EnvironmentQueryPlan plan_;
            QuerySchemaRegistry schema_;
        };

        struct PointDecision final {
            BlackboardKeyId key;
            std::shared_ptr<const CookedDecisionPlan> decision;
            EnvironmentQueryPlan query;
            QuerySchemaRegistry registry;
            Sha256Digest queryContent;
        };

        /** @brief Admits one reference-free Scene-owned query and its authored cooked decision. */
        PointDecision PreparePointDecision(Fixture &fixture) {
            const auto key = MakeIdentity<BlackboardKeyId>(22);
            const std::array keys{BlackboardKeyDescriptor{.key = key,
                                                          .kind = BlackboardValueKind::WorldCoordinate,
                                                          .presence = BlackboardKeyPresence::Optional}};
            auto schema = BlackboardSchema::Capture({.identity = SchemaId(), .keys = keys});
            REQUIRE(schema.HasValue());
            fixture.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
            auto registry = CaptureBuiltinQuerySchema();
            REQUIRE(registry.HasValue());
            const auto query =
                CreateBuiltinLatticeQuery(MakeIdentity<QueryId>(23), MakeIdentity<QueryStageId>(24), MakeIdentity<QueryResultSchemaId>(25),
                                          "Guard points", {.stepMillimeters = 500, .outerSteps = 1}, 8);
            REQUIRE(query.HasValue());
            const auto cookedQuery = EnvironmentQueryCookedCodec::Encode(query.Value(), registry.Value());
            REQUIRE(cookedQuery.HasValue());
            auto queryPlan = EnvironmentQueryPlan::Compile(query.Value(), registry.Value());
            REQUIRE(queryPlan.HasValue());
            const auto &result = queryPlan.Value().ResultSchema();
            const BehaviorTreeQueryOutput output{queryPlan.Value().Id(), result.id, result.version, result.itemType, key, 16};
            auto registration = Registration(fixture.probe);
            registration.contract.properties.emplace_back(MakeIdentity<BehaviorTreePropertyId>(26), "query output",
                                                          BehaviorTreePropertyKind::QueryOutput, output);
            auto providers = DecisionNodeGeneration::Capture({&registration, 1}, fixture.admission);
            REQUIRE(providers.HasValue());
            fixture.providers = std::move(providers).Value();
            auto task = TreeNode(2, DecisionControlKind::Task);
            task.properties = registration.contract.properties;
            task.requirements = {
                {.key = key, .kind = BlackboardValueKind::WorldCoordinate, .access = BlackboardKeyAccess::ReadWrite, .source = Location()}};
            auto graph = Tree(11, {TreeNode(1, DecisionControlKind::Root), task}, {Edge(1, 1, 2)});
            graph.executionTask = MakeIdentity<TaskId>(9000);
            return {key, fixture.Compile(std::span{&graph, 1}, 11), std::move(queryPlan).Value(), std::move(registry).Value(),
                    ComputeSha256(std::as_bytes(std::span{cookedQuery.Value()}))};
        }

        TEST_CASE("Cooked Scene query tasks wake on both publication boundaries and inherit agent retirement",
                  "[unit][ai][scene][decision_reservation][scheduler][eqs]") {
            const bool eventOnly = GENERATE(false, true);
            Fixture fixture;
            auto preparedDecision = PreparePointDecision(fixture);
            const auto &plan = preparedDecision.decision;
            const auto key = preparedDecision.key;
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            builder.Add({.object = {1}});
            const auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            const auto live = Runtime::RuntimeScene::Create(definition.Value(), scene.binding.scene);
            REQUIRE(live.HasValue());
            auto owner = std::make_shared<PointQueryOwner>(*live.Value(), scene.owner, std::move(preparedDecision.query),
                                                           std::move(preparedDecision.registry));
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto publication = TestSupport::Publish(runtime, scene);

            TestSupport::ExpectError(runtime.ActivateCookedDecisionsAtSafePoint({jobs, {}, {}}), AIErrors::CapabilityUnavailable);
            CHECK(runtime.TaskSchedulerAtSafePoint().HasError());
            CHECK(fixture.probe.factories == 0);
            AiSceneCookedDecisionExecution execution{jobs, {}, {}, owner};
            if (eventOnly) {
                execution.scheduler.mode = AiSchedulingMode::BestEffort;
                execution.agent.intervalTicks = 0;
                execution.timing = AiCookedDecisionTiming::BestEffortReactive;
            }
            REQUIRE(runtime.ActivateCookedDecisionsAtSafePoint(std::move(execution)).HasValue());
            CHECK(owner->bindings == 1);
            CHECK(owner->admissions == 0);
            const auto scheduler = runtime.TaskSchedulerAtSafePoint();
            REQUIRE(scheduler.HasValue());
            REQUIRE(scheduler.Value()->EvaluateAtDecision(1).HasValue());
            REQUIRE(scheduler.Value()->CommitAtIntentDispatch(1).HasValue());
            CHECK(owner->admissions == 1);
            CHECK(fixture.probe.factories == 0);
            REQUIRE(owner->observed);
            CHECK_FALSE(owner->observed->IsCancellationRequested());
            REQUIRE(owner->task);
            const auto agent = owner->observed->agent;
            bool published = false;
            std::size_t resumedSlices{};
            for (std::uint64_t tick = 1; tick < 16 && !published; ++tick) {
                if (tick != 1) {
                    const auto sync = runtime.SyncDecisionQueryAtBlackboardSync(*owner->task, live.Value()->View(), tick);
                    REQUIRE(sync.HasValue());
                    published = sync.Value();
                    const auto snapshot = runtime.CaptureDecisionSnapshotAtSafePoint(agent);
                    REQUIRE(snapshot.HasValue());
                    const auto value = snapshot.Value().Read(key);
                    REQUIRE(value.HasValue());
                    CHECK(value.Value().has_value() == published);
                    if (published)
                        CHECK(std::holds_alternative<Math::WorldCoordinate64>(std::get<BlackboardScalarValue>(*value.Value())));
                    const auto decision = scheduler.Value()->EvaluateAtDecision(tick);
                    REQUIRE(decision.HasValue());
                    CHECK(decision.Value().failed == 0);
                    if (eventOnly && tick <= 3)
                        CHECK(decision.Value().evaluated == 0);
                    resumedSlices += decision.Value().evaluated;
                    REQUIRE(scheduler.Value()->CommitAtIntentDispatch(tick).HasValue());
                }
                AiWorkBudget budget{tick < 3 ? 0U : 128U};
                owner->task->AdvanceQuery(tick, {budget, budget, budget});
            }
            REQUIRE(published);
            if (eventOnly)
                CHECK(resumedSlices == 2);
            CHECK_FALSE(owner->task->ActiveAgent().IsValid());
            runtime.BeginShutdown();
            CHECK(owner->observed->IsCancellationRequested());
        }

        /** @brief Hashes explicitly selected fixture values without serializing host memory layout. */
        Sha256Digest HashReplayValues(std::initializer_list<std::uint64_t> values) {
            Sha256Builder builder;
            for (const auto value : values) {
                std::array<std::byte, sizeof(value)> bytes{};
                for (std::size_t i = 0; i < bytes.size(); ++i)
                    bytes[i] = static_cast<std::byte>((value >> (i * 8)) & 0xffU);
                REQUIRE(builder.Update(bytes));
            }
            return builder.Finalize();
        }

        constexpr std::size_t BoundaryIndex(const AiReplayBoundary boundary) noexcept {
            return static_cast<std::size_t>(boundary);
        }

        struct PointReplay final {
            static constexpr std::size_t TickCount = 6;
            AiReplayManifest manifest;
            std::array<AiReplayTick, TickCount> ticks;
            std::array<std::shared_ptr<const DecisionStepTrace>, TickCount> traces;
            std::array<std::size_t, TickCount> evaluatedWork{};
            std::size_t committedQueries{};
        };

        /** @brief Projects the actual owner-issued coordinate or explicit absence at query publication. */
        Sha256Digest HashPointQueryOutput(const BlackboardSnapshot &snapshot, const BlackboardKeyId key, const bool committed) {
            const auto value = snapshot.Read(key);
            REQUIRE(value.HasValue());
            if (!value.Value())
                return HashReplayValues({false, committed});
            const auto point = std::get<Math::WorldCoordinate64>(std::get<BlackboardScalarValue>(*value.Value()));
            const auto [x, y, z] = point.Millimeters();
            return HashReplayValues(
                {true, committed, static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y), static_cast<std::uint64_t>(z)});
        }

        /** @brief Runs fixed sync, query advance and decision phases on independent real Scene and AI owners. */
        PointReplay ReplayPointDecision(const bool captureDecisionEvidence, const bool changeLastSceneInput = false,
                                        const std::uint64_t runtimeIdentity = 7) {
            Fixture fixture;
            auto setup = PreparePointDecision(fixture);
            auto selected = SceneFixture(fixture, *setup.decision);
            selected.descriptor.cookedPlan = setup.decision;
            selected.binding.incarnation = AiRuntimeIncarnation::Create(runtimeIdentity).Value();
            selected.binding.scene = Runtime::SceneRuntimeId{runtimeIdentity};
            selected.owner.runtime = selected.binding.scene;
            selected.agent.owner = selected.owner;
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            builder.Add({.object = {1}, .components = {.aiAgent = selected.agent.agent, .aiController = selected.agent.controller}});
            const auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto live = Runtime::RuntimeScene::Create(definition.Value(), selected.binding.scene);
            REQUIRE(live.HasValue());
            using enum AiReplayBoundary;
            constexpr auto Captured = AiReplayCoverage::Deterministic;
            const auto source = live.Value()->View();
            PointReplay replay;
            auto &profile = replay.manifest.boundaries;
            profile[BoundaryIndex(SceneInput)] = {Captured, 1,
                                                  HashReplayValues({source.DefinitionId().value, source.DefinitionRevision().value})};
            const auto queryVersion = setup.query.ResultSchema().version;
            profile[BoundaryIndex(QueryPublication)] = {Captured, queryVersion, setup.queryContent};
            profile[BoundaryIndex(CanonicalAi)] = {Captured, AiCanonicalRecordVersion, setup.decision->Digest()};
            if (captureDecisionEvidence) {
                replay.manifest.decisionSubject = selected.agent.agent.agent;
                profile[BoundaryIndex(Decision)] = {Captured, DecisionTraceHeader::CurrentVersion, setup.decision->Digest()};
            }
            auto owner =
                std::make_shared<PointQueryOwner>(*live.Value(), selected.owner, std::move(setup.query), std::move(setup.registry));
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = AiSceneRuntime::Create({}, AiSceneCookedDecisionExecution{jobs, {}, {}, owner}).Value();
            auto publication = TestSupport::Publish(runtime, selected);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            REQUIRE(owner->task);
            constexpr std::size_t CanonicalBudget = 4096;
            for (std::size_t index = 0; index < replay.ticks.size(); ++index) {
                auto &frame = replay.ticks[index];
                frame.tick = index + 1;
                if (changeLastSceneInput && frame.tick == replay.ticks.size()) {
                    Runtime::SceneCommandBuffer commands;
                    commands.SetLocalTransform(selected.owner, {.translation = {4, 0, 0}});
                    REQUIRE(live.Value()->Commit(commands).HasValue());
                }
                const auto scene = live.Value()->View();
                const auto translation = scene.Get(selected.owner).Value().localTransform->translation;
                frame.observations[BoundaryIndex(SceneInput)] =
                    AiReplayObservation{{scene.DefinitionRevision().value, scene.StructuralRevision()},
                                        HashReplayValues({std::bit_cast<std::uint32_t>(translation.x),
                                                          std::bit_cast<std::uint32_t>(translation.y),
                                                          std::bit_cast<std::uint32_t>(translation.z)})};
                const auto sync = runtime.SyncDecisionQueryAtBlackboardSync(*owner->task, scene, frame.tick);
                REQUIRE(sync.HasValue());
                replay.committedQueries += sync.Value() ? 1 : 0;
                AiWorkBudget budget{128};
                owner->task->AdvanceQuery(frame.tick, {budget, budget, budget});
                const auto snapshot = runtime.CaptureDecisionSnapshotAtSafePoint(agent);
                REQUIRE(snapshot.HasValue());
                const auto revision = snapshot.Value().CapturedRevision();
                frame.observations[BoundaryIndex(QueryPublication)] =
                    AiReplayObservation{{queryVersion, revision}, HashPointQueryOutput(snapshot.Value(), setup.key, sync.Value())};
                if (captureDecisionEvidence)
                    REQUIRE(runtime.ArmDecisionReplayAtSafePoint(agent, frame.tick).HasValue());
                const auto evaluated = scheduler->EvaluateAtDecision(frame.tick);
                REQUIRE(evaluated.HasValue());
                REQUIRE(evaluated.Value().failed == 0);
                replay.evaluatedWork[index] = evaluated.Value().workUnits;
                if (captureDecisionEvidence) {
                    TestSupport::ExpectError(runtime.CaptureDecisionReplayAtSafePoint(scene, frame.tick, CanonicalBudget),
                                             AIErrors::SchedulerPhaseInvalid);
                }
                REQUIRE(scheduler->CommitAtIntentDispatch(frame.tick).HasValue());
                const auto digest = [&]() {
                    if (!captureDecisionEvidence) {
                        const auto state = runtime.CaptureCanonicalState(scene, CanonicalBudget);
                        REQUIRE(state.HasValue());
                        return HashAiReplayCanonicalState(state.Value(), CanonicalBudget);
                    }
                    TestSupport::ExpectError(runtime.CaptureDecisionReplayAtSafePoint({}, frame.tick, CanonicalBudget),
                                             AIErrors::CanonicalRestoreStale);
                    TestSupport::ExpectError(runtime.CaptureDecisionReplayAtSafePoint(scene, frame.tick, 0),
                                             AIErrors::CanonicalStateInvalid);
                    const auto capture = runtime.CaptureDecisionReplayAtSafePoint(scene, frame.tick, CanonicalBudget);
                    REQUIRE(capture.HasValue());
                    frame.decision = capture.Value().Decision();
                    replay.traces[index] = capture.Value().trace;
                    TestSupport::ExpectError(runtime.CaptureDecisionReplayAtSafePoint(scene, frame.tick, CanonicalBudget),
                                             AIErrors::CanonicalRestoreStale);
                    return HashAiReplayCanonicalState(capture.Value().canonical, CanonicalBudget);
                }();
                REQUIRE(digest.HasValue());
                frame.observations[BoundaryIndex(CanonicalAi)] =
                    AiReplayObservation{{scene.DefinitionRevision().value, frame.tick}, digest.Value()};
            }
            CHECK(owner->admissions == 1);
            return replay;
        }

        TEST_CASE("Scene query and decision replay diagnoses the first changed boundary", "[unit][ai][qualification][replay]") {
            using enum AiReplayBoundary;
            const auto baseline = ReplayPointDecision(true);
            const auto repeated = ReplayPointDecision(true, false, 8);
            const auto withoutTrace = ReplayPointDecision(false, false, 9);
            const auto compare = [&](const PointReplay &other) {
                return CompareAiReplayEvidence(baseline.manifest, baseline.ticks, other.manifest, other.ticks);
            };
            REQUIRE(compare(repeated).HasValue());
            CHECK_FALSE(compare(repeated).Value());
            CHECK(baseline.traces.front()->Header().blackboard.agent != repeated.traces.front()->Header().blackboard.agent);
            auto noEvidence = baseline.ticks;
            for (auto &tick : noEvidence)
                tick.decision.reset();
            const auto timing = CompareAiReplayEvidence(withoutTrace.manifest, noEvidence, withoutTrace.manifest, withoutTrace.ticks);
            REQUIRE(timing.HasValue());
            CHECK_FALSE(timing.Value());
            CHECK(baseline.evaluatedWork == withoutTrace.evaluatedWork);
            CHECK(baseline.committedQueries == 1);
            CHECK(baseline.traces.back()->Header().outcome == AiTaskState::Succeeded);
            CHECK(baseline.ticks.front().observations[BoundaryIndex(CanonicalAi)]->canonical !=
                  baseline.ticks.back().observations[BoundaryIndex(CanonicalAi)]->canonical);

            SECTION("first difference identifies the changed committed Scene boundary") {
                const auto changedInput = compare(ReplayPointDecision(true, true));
                REQUIRE(changedInput.HasValue());
                REQUIRE(changedInput.Value());
                CHECK(changedInput.Value()->boundary == SceneInput);
                CHECK(changedInput.Value()->tick == PointReplay::TickCount);
                REQUIRE(changedInput.Value()->recorded);
                CHECK(changedInput.Value()->recorded->generation ==
                      baseline.ticks.back().observations[BoundaryIndex(SceneInput)]->stamp.generation);
            }
            SECTION("stale traces and undeclared observations reject before comparison") {
                auto malformed = repeated;
                ++malformed.ticks.back().decision->blackboardRevision;
                TestSupport::ExpectError(compare(malformed), AIErrors::CanonicalStateInvalid);
                malformed = repeated;
                malformed.ticks.front().observations[BoundaryIndex(Physics)] =
                    *malformed.ticks.front().observations[BoundaryIndex(SceneInput)];
                TestSupport::ExpectError(compare(malformed), AIErrors::CanonicalStateInvalid);
            }
            SECTION("valid unequal windows report the exact first missing tick") {
                const auto incomplete = CompareAiReplayEvidence(baseline.manifest, baseline.ticks, repeated.manifest,
                                                                std::span{repeated.ticks}.first(PointReplay::TickCount - 1));
                REQUIRE(incomplete.HasValue());
                REQUIRE(incomplete.Value());
                CHECK(incomplete.Value()->tick == PointReplay::TickCount);
                CHECK_FALSE(incomplete.Value()->boundary);
            }
            SECTION("manifest versions and content must agree before any tick comparison") {
                auto incompatible = repeated;
                ++incompatible.manifest.boundaries[BoundaryIndex(Decision)].schemaVersion;
                TestSupport::ExpectError(compare(incompatible), AIErrors::CanonicalSchemaUnsupported);
                TestSupport::ExpectError(CompareAiReplayEvidence(incompatible.manifest, incompatible.ticks, incompatible.manifest,
                                                                 incompatible.ticks),
                                         AIErrors::CanonicalSchemaUnsupported);
                incompatible = repeated;
                incompatible.manifest.boundaries[BoundaryIndex(QueryPublication)].content = HashReplayValues({12});
                TestSupport::ExpectError(compare(incompatible), AIErrors::CanonicalSchemaUnsupported);
            }
            SECTION("nondeterministic boundaries remain explicitly excluded") {
                auto profile = baseline.manifest;
                profile.boundaries[BoundaryIndex(Navigation)] = {AiReplayCoverage::Nondeterministic, 1, HashReplayValues({17})};
                const auto excluded = CompareAiReplayEvidence(profile, baseline.ticks, profile, repeated.ticks);
                REQUIRE(excluded.HasValue());
                CHECK_FALSE(excluded.Value());
                auto falselyClaimed = repeated.ticks;
                falselyClaimed.front().observations[BoundaryIndex(Navigation)] =
                    *repeated.ticks.front().observations[BoundaryIndex(SceneInput)];
                TestSupport::ExpectError(CompareAiReplayEvidence(profile, baseline.ticks, profile, falselyClaimed),
                                         AIErrors::CanonicalStateInvalid);
            }
        }

        TEST_CASE("Cooked controller publication rejects a provider retired after Scene preparation",
                  "[unit][ai][scene][decision_reservation][composition]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            const auto plan = fixture.Compile(sources, 1);
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto prepared = runtime.PrepareScene(scene.binding, std::array{scene.agent}, std::array{scene.descriptor});
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value()->ValidatePublication().HasValue());
            fixture.admission->store(false);
            TestSupport::ExpectError(prepared.Value()->ValidatePublication(), AIErrors::ControllerDescriptorIncompatible);
            CHECK(runtime.Snapshot().HasError());
            CHECK(fixture.probe.factories == 0);
        }

        /** @brief Builds the two-option plan and gives the learned task an optional eligibility requirement. */
        auto BuildUtilityPolicyPlan(Fixture &fixture, TaskId learned, bool eligible, BlackboardKeyId feature = {}) {
            auto source = UtilitySource();
            auto &utility = std::get<UtilityDecisionSource>(source.body);
            const auto scalar = utility.requirements.front().key;
            const auto absent = MakeIdentity<BlackboardKeyId>(102);
            std::vector keys{BlackboardKeyDescriptor{.key = scalar,
                                                          .kind = BlackboardValueKind::Scalar,
                                                          .defaultValue = BlackboardValue{BlackboardScalarValue{0.5}}},
                                  BlackboardKeyDescriptor{.key = absent,
                                                          .kind = BlackboardValueKind::Scalar,
                                                          .presence = BlackboardKeyPresence::Optional}};
            if (feature.IsValid()) {
                source.executionTask = MakeIdentity<TaskId>(9000);
                keys.push_back({.key = feature,
                                .kind = BlackboardValueKind::SignedInteger,
                                .defaultValue = BlackboardValue{BlackboardScalarValue{std::int64_t{12}}}});
            }
            auto schema = BlackboardSchema::Capture({.identity = SchemaId(), .keys = keys});
            REQUIRE(schema.HasValue());
            fixture.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
            const auto fallback = utility.options.front().option.action;
            auto preference = utility.options.front();
            preference.option.id = preference.provider.id = Node(3);
            preference.option.action = learned;
            preference.option.priority = 0.5;
            if (!eligible) {
                auto requirement = utility.requirements.front();
                requirement.key = absent;
                utility.requirements.push_back(requirement);
                preference.option.considerations.front().key = absent;
            }
            utility.options.push_back(std::move(preference));
            return std::pair{fixture.Compile(std::span{&source, 1}, 1), fallback};
        }

        /** @brief Prepares one model row and its exact registered task declaration. */
        auto CookUtilityPolicy(TaskId modelTask) {
            AiCapabilityDescriptorRegistry declarations;
            REQUIRE(declarations.Register({modelTask, SchemaId()}).HasValue());
            const auto tasks = declarations.Freeze();
            const auto target = MakeIdentity<LocalPolicyTargetId>(1);
            const std::array targets{target};
            const std::array features{MakeIdentity<LocalPolicyFeatureId>(1)};
            const std::array<std::int16_t, 1> weights{1};
            const std::array rows{LocalPolicyRowSource{modelTask, 0, weights}};
            auto model = LocalLearnedPolicy::Cook({MakeIdentity<LocalPolicyModelId>(1), 1, targets, features, rows},
                                                        {target, true, sizeof(LocalLearnedPolicy), 3}, tasks, SchemaId());
            REQUIRE(model.HasValue());
            return std::pair{std::move(model).Value(), tasks};
        }

        /** @brief Verifies selection and continuation without allowing inference to replace a running task. */
        void CheckUtilityPolicySteps(const auto &step, const auto &probe, TaskId expectedTask, const AiWorkBudget &modelBudget) {
            const auto first = step(1);
            REQUIRE(first.HasValue());
            CHECK(first.Value() == AiTaskState::Running);
            REQUIRE(probe.calls.size() == 1);
            CHECK(probe.calls.front() == "start:" + std::to_string(expectedTask.Value()));
            const auto spent = modelBudget.Remaining();
            const auto next = step(2);
            REQUIRE(next.HasValue());
            CHECK(next.Value() == AiTaskState::Succeeded);
            CHECK(modelBudget.Remaining() == spent);
            CHECK(probe.starts == 1);
            CHECK(probe.resumes == 1);
        }

        /** @brief Binds the Utility plan to a real Scene task and keeps its reservation alive in the instance. */
        auto BindUtilityPolicy(Fixture &fixture, AiSceneRuntime &runtime, AgentHandle agent,
                               const std::shared_ptr<const CookedDecisionPlan> &plan) {
            const auto parentTask = MakeIdentity<TaskId>(9000);
            const auto parent = runtime.StartTaskAtSafePoint(agent, parentTask);
            REQUIRE(parent.HasValue());
            auto reserved = runtime.ReserveDecisionTaskSlotsAtSafePoint(agent, static_cast<std::uint32_t>(plan->Storage().taskSlots));
            REQUIRE(reserved.HasValue());
            auto instance = DecisionGraphInstance::Create(plan, fixture.providers, std::move(reserved).Value(),
                                                          {.taskDefinition = parentTask, .task = parent.Value(), .agent = agent});
            REQUIRE(instance.HasValue());
            return std::move(instance).Value();
        }

        TEST_CASE("Scene-owned Utility runs an eligible learned task or its authored fallback",
                  "[ai][decision_reservation][local-policy]") {
            const auto scenario = GENERATE("preferred", "stale tensor", "no work", "shared work", "scheduler work", "unknown action",
                                           "cancelled", "ineligible");
            Fixture fixture;
            const std::string_view choice = scenario;
            const auto learned = MakeIdentity<TaskId>(5003);
            const auto [plan, fallback] = BuildUtilityPolicyPlan(fixture, learned, choice != "ineligible");
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            const auto publication = TestSupport::Publish(runtime, SceneFixture(fixture, *plan));
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            const auto instance = BindUtilityPolicy(fixture, runtime, agent, plan);
            const auto snapshot = runtime.CaptureDecisionSnapshotAtSafePoint(agent);
            REQUIRE(snapshot.HasValue());

            const auto modelTask = choice == "unknown action" ? MakeIdentity<TaskId>(7000) : learned;
            const auto [model, tasks] = CookUtilityPolicy(modelTask);
            const std::array tensor{LocalPolicyFeature{model.Features().front(), 10}};
            const auto revision = snapshot.Value().CapturedRevision();
            AiWorkBudget modelBudget{choice == "no work" ? 0U : 3U};
            AiWorkBudget agentBudget{100};
            AiWorkBudget globalBudget{100};
            const auto graphWork = plan->MaximumDecisionWorkUnits();
            REQUIRE(graphWork);
            AiWorkBudget schedulerBudget{2 * *graphWork + 3};
            auto &utilityModelBudget = choice == "shared work" ? agentBudget : modelBudget;
            CancellationSource inference;
            if (choice == "cancelled")
                inference.RequestCancellation();
            const LocalPolicyUtilityInput policy{model,
                                                 tasks,
                                                 tensor,
                                                 model.Digest(),
                                                 model.Target(),
                                                 1,
                                                 revision + (choice == "stale tensor" ? 1 : 0),
                                                 choice == "scheduler work" ? schedulerBudget : utilityModelBudget,
                                                 inference.Token()};
            const auto step = [&](const std::uint64_t tick) {
                const DecisionGraphStepInput input{tick, snapshot.Value().Binding(), snapshot.Value(), {}, &agentBudget, &globalBudget};
                if (choice != "scheduler work")
                    return instance->StepWithLocalPolicy(input, policy);
                const auto stepped = instance->StepBudgetedWithLocalPolicy(input, schedulerBudget, policy);
                REQUIRE(stepped.HasValue());
                REQUIRE(stepped.Value());
                return Result<AiTaskState>::Success(*stepped.Value());
            };
            CheckUtilityPolicySteps(step, fixture.probe, choice == "preferred" ? learned : fallback, modelBudget);
            if (choice == "scheduler work")
                CHECK(schedulerBudget.Remaining() == 3);
        }

        TEST_CASE("Scene Utility falls back when only the graph fits the remaining work allowance",
                  "[ai][decision_reservation][local-policy]") {
            const auto graphFits = GENERATE(true, false);
            Fixture fixture;
            const auto learned = MakeIdentity<TaskId>(5003);
            const auto feature = MakeIdentity<BlackboardKeyId>(104);
            const auto [plan, fallback] = BuildUtilityPolicyPlan(fixture, learned, true, feature);
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            auto next = scene.agent;
            next.agent.agent = MakeIdentity<AgentId>(2);
            ++next.owner.entity.index;

            auto [model, tasks] = CookUtilityPolicy(learned);
            const auto graphWork = plan->MaximumDecisionWorkUnits().value();
            const auto modelWork = model.RequiredWork() + std::bit_width(MaximumBlackboardKeys);
            const auto binding = std::make_shared<const AiSceneLocalPolicyBinding>(
                AiSceneLocalPolicyBinding{std::make_shared<const LocalLearnedPolicy>(std::move(model)), tasks, {feature}});
            const auto policies =
                std::make_shared<const AiSceneLocalPolicyCatalog>(AiSceneLocalPolicyCatalog{{scene.descriptor.controller, binding}});
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto publication = runtime.PrepareScene(scene.binding, std::array{scene.agent, next}, std::array{scene.descriptor});
            REQUIRE(publication.HasValue());
            REQUIRE(publication.Value()->ValidatePublication().HasValue());
            publication.Value()->Publish();
            const auto globalWork = graphWork + modelWork + (graphFits ? graphWork : graphWork - 1);
            REQUIRE(runtime.ActivateCookedDecisionsAtSafePoint(
                               {.jobs = jobs,
                                .scheduler = {.workUnitsPerTick = globalWork, .mode = AiSchedulingMode::BestEffort},
                                .agent = {.workUnitsPerTick = graphWork + modelWork},
                                .timing = AiCookedDecisionTiming::BestEffortReactive,
                                .localPolicies = policies}).HasValue());
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            const auto tick = scheduler->EvaluateAtDecision(1);
            REQUIRE(tick.HasValue());
            CHECK(tick.Value().failed == 0);
            CHECK(tick.Value().exhausted == (graphFits ? 0 : 1));
            REQUIRE(fixture.probe.calls.size() == (graphFits ? 2 : 1));
            CHECK(fixture.probe.calls.front() == "start:" + std::to_string(learned.Value()));
            if (graphFits)
                CHECK(fixture.probe.calls.back() == "start:" + std::to_string(fallback.Value()));
            REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());

            if (graphFits) {
                const auto resumed = scheduler->EvaluateAtDecision(2);
                REQUIRE(resumed.HasValue());
                CHECK(resumed.Value().failed == 0);
                CHECK(resumed.Value().exhausted == 0);
                CHECK(resumed.Value().workUnits == 2 * graphWork);
                REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());
            }
        }

        TEST_CASE("Scene fixed ticks feed a verified model from the current integer Blackboard",
                  "[ai][decision_reservation][local-policy]") {
            const bool outOfRange = GENERATE(false, true);
            enum class Callback { Normal, DiscardTrace, Fail };
            const auto callback = GENERATE(Callback::Normal, Callback::DiscardTrace, Callback::Fail);
            const bool discardFromProvider = callback == Callback::DiscardTrace;
            Fixture fixture;
            auto source = UtilitySource();
            source.executionTask = MakeIdentity<TaskId>(9000);
            auto &utility = std::get<UtilityDecisionSource>(source.body);
            const auto scalar = utility.requirements.front().key;
            const auto feature = MakeIdentity<BlackboardKeyId>(104);
            const std::array keys{BlackboardKeyDescriptor{.key = scalar,
                                                          .kind = BlackboardValueKind::Scalar,
                                                          .defaultValue = BlackboardValue{BlackboardScalarValue{0.5}}},
                                  BlackboardKeyDescriptor{.key = feature,
                                                          .kind = BlackboardValueKind::SignedInteger,
                                                          .defaultValue = BlackboardValue{
                                                              BlackboardScalarValue{std::int64_t{outOfRange ? 32768 : 12}}}}};
            auto schema = BlackboardSchema::Capture({.identity = SchemaId(), .keys = keys});
            REQUIRE(schema.HasValue());
            fixture.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
            const auto fallback = utility.options.front().option.action;
            const auto learned = MakeIdentity<TaskId>(5003);
            auto preference = utility.options.front();
            preference.option.id = preference.provider.id = Node(3);
            preference.option.action = learned;
            preference.option.priority = 0.5;
            utility.options.push_back(std::move(preference));
            const auto plan = fixture.Compile(std::span{&source, 1}, 1);
            auto scene = SceneFixture(fixture, *plan);
            scene.descriptor.cookedPlan = plan;
            const auto graphWork = plan->MaximumDecisionWorkUnits();
            REQUIRE(graphWork);

            AiCapabilityDescriptorRegistry declarations;
            REQUIRE(declarations.Register({learned, SchemaId()}).HasValue());
            const auto tasks = declarations.Freeze();
            const auto target = MakeIdentity<LocalPolicyTargetId>(1);
            const std::array targets{target};
            const std::array features{MakeIdentity<LocalPolicyFeatureId>(1)};
            const std::array<std::int16_t, 1> weights{1};
            const std::array rows{LocalPolicyRowSource{learned, 0, weights}};
            auto model = LocalLearnedPolicy::Cook({MakeIdentity<LocalPolicyModelId>(1), 1, targets, features, rows},
                                                  {target, true, sizeof(LocalLearnedPolicy), 3}, tasks, SchemaId());
            REQUIRE(model.HasValue());
            const auto binding = std::make_shared<const AiSceneLocalPolicyBinding>(
                AiSceneLocalPolicyBinding{std::make_shared<const LocalLearnedPolicy>(std::move(model).Value()), tasks, {feature}});
            const auto policies =
                std::make_shared<const AiSceneLocalPolicyCatalog>(AiSceneLocalPolicyCatalog{{scene.descriptor.controller, binding}});
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            const auto publication = TestSupport::Publish(runtime, scene);
            TestSupport::ExpectError(runtime.ActivateCookedDecisionsAtSafePoint(
                                         {.jobs = jobs, .agent = {.workUnitsPerTick = *graphWork}, .localPolicies = policies}),
                                     AIErrors::SchedulerBudgetExhausted);
            CHECK(fixture.probe.factories == 0);
            REQUIRE(runtime.ActivateCookedDecisionsAtSafePoint({.jobs = jobs, .localPolicies = policies}).HasValue());
            auto *scheduler = runtime.TaskSchedulerAtSafePoint().Value();
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{7}, Runtime::SceneDefinitionRevision{1}};
            builder.Add(Runtime::RuntimeEntityDefinition{.object = Runtime::SceneObjectId{1}});
            const auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            const auto live = Runtime::RuntimeScene::Create(definition.Value(), scene.binding.scene);
            REQUIRE(live.HasValue());
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            const auto expected = outOfRange ? fallback : learned;
            TestSupport::ExpectError(runtime.ArmDecisionReplayAtSafePoint(agent, 1), AIErrors::DecisionAssetActivationInvalid);
            REQUIRE(runtime.ArmDecisionTraceAtSafePoint(agent, 1).HasValue());
            if (callback == Callback::Fail) {
                const auto error = MakeError(AIErrors::CapabilityUnavailable, "Fixture Utility task callback failed.");
                fixture.probe.startOutcome = Result<AiTaskState>::Failure(error);
                const auto evaluated = scheduler->EvaluateAtDecision(1);
                REQUIRE(evaluated.HasValue());
                CHECK_FALSE(evaluated.Value().firstFailure);
                REQUIRE(evaluated.Value().firstAdvisoryFailure);
                REQUIRE(scheduler->CommitAtIntentDispatch(1).HasValue());
                const auto diagnostic = runtime.ReadDecisionFailureAtSafePoint(live.Value()->View());
                REQUIRE(diagnostic.HasValue());
                REQUIRE(diagnostic.Value());
                CHECK(diagnostic.Value()->origin.asset == plan->Root());
                CHECK(diagnostic.Value()->origin.node == (outOfRange ? Node(2) : Node(3)));
                CHECK(diagnostic.Value()->error.code.Value() == error.code.Value());
                CHECK(diagnostic.Value()->error.message == error.message);
                REQUIRE_FALSE(fixture.probe.calls.empty());
                CHECK(fixture.probe.calls.front() == "start:" + std::to_string(expected.Value()));
                CHECK(fixture.probe.cleanups == 1);
                return;
            }
            if (discardFromProvider)
                fixture.probe.onStart = [&] {
                    runtime.DiscardDecisionReplayAtSafePoint(agent, 1);
                };
            for (const std::uint64_t tick : {1, 2}) {
                REQUIRE(scheduler->EvaluateAtDecision(tick).Value().failed == 0);
                REQUIRE(scheduler->CommitAtIntentDispatch(tick).HasValue());
                if (tick == 1) {
                    const auto view = live.Value()->View();
                    if (discardFromProvider) {
                        TestSupport::ExpectError(runtime.TakeDecisionTraceAtSafePoint(view, agent, tick), AIErrors::CanonicalRestoreStale);
                        continue;
                    }
                    TestSupport::ExpectError(runtime.CaptureDecisionReplayAtSafePoint(view, tick, 4096), AIErrors::CanonicalStateInvalid);
                    const auto captured = runtime.TakeDecisionTraceAtSafePoint(view, agent, tick);
                    REQUIRE(captured.HasValue());
                    CHECK(captured.Value()->Header().tick == tick);
                    CHECK(captured.Value()->Header().planDigest == plan->Digest());
                    CHECK(captured.Value()->DroppedRecords() == 0);
                    CHECK(std::ranges::any_of(captured.Value()->Records(), [expected](const DecisionStepRecord &record) {
                        return record.kind == DecisionStepRecordKind::BehaviorNode && record.task == expected;
                    }));
                    CHECK(std::ranges::none_of(captured.Value()->Records(), [feature](const DecisionStepRecord &record) {
                        return record.key == feature;
                    }));
                }
            }
            REQUIRE_FALSE(fixture.probe.calls.empty());
            CHECK(fixture.probe.calls.front() == "start:" + std::to_string(expected.Value()));
            CHECK(fixture.probe.starts == 1);
            CHECK(fixture.probe.resumes == 1);
        }

        TEST_CASE("Scene-owned composed decisions retire tasks before reading revoked snapshots",
                  "[unit][ai][scene][decision_reservation][composition]") {
            Fixture fixture;
            const auto family = GENERATE(DecisionPlanKind::BehaviorTree, DecisionPlanKind::StateMachine, DecisionPlanKind::Utility);
            std::vector sources{TaskTree(1)};
            if (family == DecisionPlanKind::StateMachine)
                sources = {CallingMachine(1, 2, DecisionPlanKind::BehaviorTree), TaskTree(2)};
            if (family == DecisionPlanKind::Utility) {
                fixture.schema = TestSupport::MakeSchema(100, 1, BlackboardValueKind::Scalar, BlackboardScalarValue{0.5});
                sources = {UtilitySource()};
            }
            auto plan = fixture.Compile(sources, 1);
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto scene = SceneFixture(fixture, *plan);
            auto publication = TestSupport::Publish(runtime, scene);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            const auto task = TestSupport::MakeIdentity<TaskId>(9000);
            auto native = runtime.StartTaskAtSafePoint(agent, task);
            REQUIRE(native.HasValue());
            auto reserved = runtime.ReserveDecisionTaskSlotsAtSafePoint(agent, static_cast<std::uint32_t>(plan->Storage().taskSlots));
            REQUIRE(reserved.HasValue());
            const auto first = reserved.Value().FirstSlot();
            const auto token = reserved.Value().Cancellation();
            CancellationSource operation;
            CancellationSource capability;
            AiTaskOperationContext parent{task, native.Value(), agent, operation.Token(), capability.Token()};
            auto created = DecisionGraphInstance::Create(plan, fixture.providers, std::move(reserved).Value(), parent);
            REQUIRE(created.HasValue());
            auto instance = std::move(created).Value();
            auto snapshot = runtime.CaptureDecisionSnapshotAtSafePoint(agent);
            REQUIRE(snapshot.HasValue());
            AiWorkBudget agentBudget{100};
            AiWorkBudget globalBudget{100};
            auto started = instance->Step({1, snapshot.Value().Binding(), snapshot.Value(), {}, &agentBudget, &globalBudget});
            REQUIRE(started.HasValue());
            CHECK(started.Value() == AiTaskState::Running);
            REQUIRE(fixture.probe.tasks.size() == 1);
            CHECK(fixture.probe.tasks.front().slot.index >= first);
            CHECK(fixture.probe.tasks.front() != native.Value());
            SECTION("actual agent disable") {
                REQUIRE(runtime.DisableAtSafePoint(agent).HasValue());
                CHECK(snapshot.Value().Revision().HasError());
            }
            SECTION("operation ancestor") {
                operation.RequestCancellation();
            }
            SECTION("module capability ancestor") {
                capability.RequestCancellation();
            }
            auto cancelled = instance->Step({2, snapshot.Value().Binding(), snapshot.Value(), {}, &agentBudget, &globalBudget});
            REQUIRE(cancelled.HasValue());
            CHECK(cancelled.Value() == AiTaskState::Cancelled);
            CHECK(fixture.probe.starts == 1);
            CHECK(fixture.probe.resumes == 0);
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
            instance.reset();
            CHECK(token.IsCancellationRequested());
        }

        TEST_CASE("Scene graph schema rejection consumes its reservation without invoking provider factories",
                  "[unit][ai][scene][decision_reservation][composition]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            auto plan = fixture.Compile(sources, 1);
            TestSupport::RestoreHarness h;
            auto reserved = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 4);
            REQUIRE(reserved.HasValue());
            const auto token = reserved.Value().Cancellation();
            const auto task = TestSupport::MakeIdentity<TaskId>(9000);
            auto native = h.runtime.StartTaskAtSafePoint(h.handle, task);
            REQUIRE(native.HasValue());
            AiTaskOperationContext operation{task, native.Value(), h.handle};
            auto rejected = DecisionGraphInstance::Create(plan, fixture.providers, std::move(reserved).Value(), operation);
            TestSupport::ExpectError(rejected, AIErrors::DecisionAssetSchemaIncompatible);
            CHECK(fixture.probe.factories == 0);
            CHECK(token.IsCancellationRequested());
            auto next = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 1);
            REQUIRE(next.HasValue());
            CHECK(next.Value().FirstSlot() == 5);
        }
    }  // namespace
}  // namespace Horo::AI
