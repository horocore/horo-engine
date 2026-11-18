#include "AiSceneTestSupport.h"
#include "DecisionGraphCompilerTestSupport.h"

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
