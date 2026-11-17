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
