#include "AiSceneTestSupport.h"
#include "DecisionGraphCompilerTestSupport.h"
#include "Horo/AI/AIReplayEvidence.h"
#include "Horo/AI/DecisionQueryTask.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/generators/catch_generators.hpp>

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
            const auto denied = tree.Value()->StepBudgeted({1, snapshot.Value().Binding(), snapshot.Value(), {}}, insufficient);
            REQUIRE(denied.HasValue());
            CHECK_FALSE(denied.Value());
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

        TEST_CASE("Budgeted execution rejects a composed decision graph before consuming its scheduler allowance",
                  "[unit][ai][scene][decision_reservation][scheduler]") {
            Fixture fixture;
            const std::array sources{CallTree(1, 2, DecisionPlanKind::BehaviorTree), TaskTree(2)};
            const auto plan = fixture.Compile(sources, 1);
            CHECK_FALSE(plan->MaximumDecisionWorkUnits());
            auto graph = DecisionGraphInstance::Create(plan, fixture.providers, fixture.RuntimeBinding());
            REQUIRE(graph.HasValue());
            const auto blackboard = fixture.blackboard->Snapshot();
            REQUIRE(blackboard.HasValue());
            AiWorkBudget budget{32};
            const auto result = graph.Value()->StepBudgeted({1, fixture.binding, blackboard.Value(), {}}, budget);
            TestSupport::ExpectError(result, AIErrors::CapabilityUnavailable);
            CHECK(budget.Remaining() == 32);
            CHECK(fixture.probe.starts == 0);
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
            REQUIRE(scheduler->CommitAtIntentDispatch(2).HasValue());

            bool evaluationDuringAbortRejected{};
            old.probe.onAbort = [&] {
                evaluationDuringAbortRejected = scheduler->EvaluateAtDecision(3).HasError();
            };
            REQUIRE(runtime.ReplaceCookedDecisionAtSafePoint(agent, previous, replacement).HasValue());
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

        TEST_CASE("Cooked Scene query tasks bind through the selected owner and inherit agent retirement",
                  "[unit][ai][scene][decision_reservation][scheduler][eqs]") {
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
            REQUIRE(runtime.ActivateCookedDecisionsAtSafePoint({jobs, {}, {}, owner}).HasValue());
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
                    REQUIRE(scheduler.Value()->EvaluateAtDecision(tick).HasValue());
                    REQUIRE(scheduler.Value()->CommitAtIntentDispatch(tick).HasValue());
                }
                AiWorkBudget budget{128};
                owner->task->AdvanceQuery(tick, {budget, budget, budget});
            }
            REQUIRE(published);
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
            std::array<AiTaskState, TickCount> outcomes{};
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
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto publication = TestSupport::Publish(runtime, selected);
            const auto agent = runtime.Snapshot().Value().Agents().front().handle;
            const auto rootTask = MakeIdentity<TaskId>(9000);
            const auto parent = runtime.StartTaskAtSafePoint(agent, rootTask);
            REQUIRE(parent.HasValue());
            auto tasks =
                runtime.ReserveDecisionTaskSlotsAtSafePoint(agent, static_cast<std::uint32_t>(setup.decision->Storage().taskSlots));
            REQUIRE(tasks.HasValue());
            auto decision =
                DecisionGraphInstance::Create(setup.decision, std::move(tasks).Value(), {rootTask, parent.Value(), agent}, owner);
            REQUIRE(decision.HasValue());
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
                const DecisionGraphStepInput input{frame.tick, snapshot.Value().Binding(), snapshot.Value(), {}};
                auto trace = captureDecisionEvidence ? std::make_shared<DecisionStepTrace>() : nullptr;
                const auto outcome = trace ? decision.Value()->StepWithTrace(input, *trace) : decision.Value()->Step(input);
                REQUIRE(outcome.HasValue());
                replay.outcomes[index] = outcome.Value();
                if (trace) {
                    frame.decision = AiReplayDecisionObservation{replay.manifest.decisionSubject, input.current, revision, trace.get()};
                    replay.traces[index] = std::move(trace);
                }
                const auto state = runtime.CaptureCanonicalState(scene, CanonicalBudget);
                REQUIRE(state.HasValue());
                const auto digest = HashAiReplayCanonicalState(state.Value(), CanonicalBudget);
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
            CHECK(baseline.outcomes == withoutTrace.outcomes);
            CHECK(baseline.committedQueries == 1);
            CHECK(baseline.outcomes.back() == AiTaskState::Succeeded);
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
