#include "AllocationProbe.h"
#include "BehaviorTreeRuntimeTestSupport.h"

#include <algorithm>

namespace Horo::AI {
    namespace {
        using BehaviorTreeTestSupport::Executor;
        using BehaviorTreeTestSupport::Fixture;
        using BehaviorTreeTestSupport::Id;
        using BehaviorTreeTestSupport::Node;
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        TEST_CASE("Reload preserves compatible tasks and rejects null while incompatible topology aborts", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using enum AiTaskState;
            Fixture fixture({Node(1, Sequence, {2}), Node(2, Task)});
            fixture.probe->outcome[2] = Running;
            auto instance = fixture.Instance();
            REQUIRE(fixture.Evaluate(*instance, 1) == Running);
            const auto handle = fixture.probe->handles[2];
            auto retained = instance->Replace(fixture.Plan());
            REQUIRE(retained.HasValue());
            CHECK(retained.Value());
            CHECK(fixture.probe->aborts[2] == 0);
            ExpectError(instance->Replace(nullptr), AIErrors::DecisionAssetActivationInvalid);
            CHECK(fixture.Evaluate(*instance, 2) == Running);
            CHECK(fixture.probe->handles[2] == handle);
            fixture.nodes[0].operation = Selector;
            auto restarted = instance->Replace(fixture.Plan());
            REQUIRE(restarted.HasValue());
            CHECK_FALSE(restarted.Value());
            CHECK(instance->RootStatus() == Idle);
            CHECK(fixture.probe->aborts[2] == 1);
            CHECK(fixture.Evaluate(*instance, 3) == Running);
            CHECK(fixture.probe->handles[2] != handle);
        }

        TEST_CASE("Generation retirement cancellation and shutdown prevent any later callback", "[unit][ai][bt_runtime]") {
            using enum AiTaskState;
            for (const bool generation : {true, false}) {
                Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
                fixture.probe->outcome[1] = Running;
                CancellationSource cancellation;
                auto instance = fixture.Instance(1, cancellation.Token());
                REQUIRE(fixture.Evaluate(*instance, 1) == Running);
                auto snapshot = fixture.blackboard->Snapshot();
                REQUIRE(snapshot.HasValue());
                auto active = fixture.agent;
                if (generation)
                    ++active.slot.generation;
                else
                    cancellation.RequestCancellation();
                auto result = instance->Evaluate(2, snapshot.Value(), active);
                REQUIRE(result.HasValue());
                CHECK(result.Value() == Cancelled);
                CHECK(fixture.probe->terminals[1]->cancellationReason ==
                      (generation ? AiTaskCancellationReason::AgentGenerationRetired : AiTaskCancellationReason::ContextCancelled));
                ExpectError(instance->Evaluate(3, snapshot.Value(), fixture.agent), AIErrors::TaskTransitionInvalid);
                instance->Shutdown();
                CHECK(fixture.probe->aborts[1] == 1);
                CHECK(fixture.probe->cleanups[1] == 1);
                CHECK(fixture.probe->resumes[1] == 0);
            }
        }

        TEST_CASE("Precancelled trees never start tasks and destructors abort running work", "[unit][ai][bt_runtime]") {
            using enum AiTaskState;
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            auto cancelled = fixture.Instance(1, cancellation.Token());
            CHECK(fixture.Evaluate(*cancelled, 1) == Cancelled);
            CHECK(fixture.probe->starts[1] == 0);
            CHECK(fixture.probe->aborts[1] == 0);
            fixture.probe->outcome[1] = Running;
            auto running = fixture.Instance();
            REQUIRE(fixture.Evaluate(*running, 1) == Running);
            running.reset();
            CHECK(fixture.probe->terminals[1]->cancellationReason == AiTaskCancellationReason::OwnerShutdown);
            CHECK(fixture.probe->aborts[1] == 1);
            CHECK(fixture.probe->cleanups[1] == 1);
        }

        TEST_CASE("Task failures retain the original typed cause and invalid output is terminal", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            fixture.probe->failTask = 1;
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Failed);
            REQUIRE(fixture.probe->terminals[1]->failure.has_value());
            CHECK(fixture.probe->terminals[1]->failure->cause.code.Value() == AIErrors::RuntimeUnavailable.code.Value());
            CHECK(fixture.probe->cleanups[1] == 1);
            REQUIRE(instance->Restart().HasValue());
            fixture.probe->failTask = 0;
            fixture.probe->invalidOutput = true;
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Failed);
            CHECK(fixture.probe->terminals[1]->failure->kind == AiTaskFailureKind::InvalidOutput);
        }

        TEST_CASE("Provider service failure aborts previously running branches without losing the error", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            Fixture fixture({Node(1, Parallel, {2, 3}), Node(2, Task), Node(3, Task)},
                            {{Id(4), Id(3), BehaviorTreeServiceMode::Periodic, 1}});
            fixture.probe->outcome[2] = AiTaskState::Running;
            fixture.probe->failService = 4;
            auto instance = fixture.Instance();
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            ExpectError(instance->Evaluate(1, snapshot.Value(), fixture.agent), AIErrors::RuntimeUnavailable);
            CHECK(fixture.probe->aborts[2] == 1);
            CHECK(fixture.probe->cleanups[2] == 1);
            CHECK(fixture.probe->starts[3] == 0);
        }

        TEST_CASE("Task generation exhaustion cannot wrap or admit replacement work", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            Fixture fixture({Node(1, Sequence, {2, 3}), Node(2, Task), Node(3, Task)});
            auto instance = fixture.Instance(std::numeric_limits<std::uint32_t>::max());
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            ExpectError(instance->Evaluate(1, snapshot.Value(), fixture.agent), AIErrors::GenerationExhausted);
            CHECK(fixture.probe->starts[2] == 1);
            CHECK(fixture.probe->starts[3] == 0);
            CHECK(fixture.probe->cleanups[2] == 1);
        }

        TEST_CASE("Invalid clocks and stale blackboards reject evaluation without touching tasks", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            fixture.probe->outcome[1] = AiTaskState::Running;
            auto instance = fixture.Instance();
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            ExpectError(instance->Evaluate(0, snapshot.Value(), fixture.agent), AIErrors::TaskTransitionInvalid);
            REQUIRE(fixture.Evaluate(*instance, 2) == AiTaskState::Running);
            ExpectError(instance->Evaluate(1, snapshot.Value(), fixture.agent), AIErrors::TaskTransitionInvalid);
            REQUIRE(fixture.blackboard->TeardownAtBlackboardSync().HasValue());
            ExpectError(instance->Evaluate(3, snapshot.Value(), fixture.agent), AIErrors::BlackboardInstanceStale);
            CHECK(fixture.probe->starts[1] == 1);
            CHECK(fixture.probe->resumes[1] == 0);
        }

        TEST_CASE("The maximum-depth tree executes iteratively and lower project limits reject it", "[unit][ai][bt_runtime]") {
            std::vector<BehaviorTreeExecutionNode> nodes;
            for (std::size_t id = 1; id < BehaviorTreeExecutionLimits::HardNodes; ++id)
                nodes.push_back(Node(id, BehaviorTreeOperation::Inverter, {id + 1}));
            nodes.push_back(Node(BehaviorTreeExecutionLimits::HardNodes, BehaviorTreeOperation::Task));
            Fixture fixture(std::move(nodes));
            auto plan = fixture.Plan();
            CHECK(plan->Depth() == BehaviorTreeExecutionLimits::HardNodes);
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Failed);
            CHECK(fixture.probe->starts[BehaviorTreeExecutionLimits::HardNodes] == 1);
            ExpectError(BehaviorTreeExecutionPlan::Compile(fixture.Bindings(), Id(1), fixture.nodes, {}, {.maximumDepth = 64}),
                        AIErrors::BehaviorTreeLimitExceeded);
        }

        TEST_CASE("Exact blackboard publication fences and incompatible schema reload preserve running work", "[unit][ai][bt_runtime]") {
            using enum AiTaskState;
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            fixture.probe->outcome[1] = Running;
            auto instance = fixture.Instance();
            REQUIRE(fixture.Evaluate(*instance, 1) == Running);
            auto foreign =
                BlackboardInstance::Create({fixture.agent, fixture.schema->Identity(), fixture.schema->Version(), 2, 2}, fixture.schema);
            REQUIRE(foreign.HasValue());
            auto snapshot = foreign.Value()->Snapshot();
            REQUIRE(snapshot.HasValue());
            ExpectError(instance->Evaluate(2, snapshot.Value(), fixture.agent), AIErrors::BlackboardInstanceInvalid);
            Fixture replacement({Node(1, BehaviorTreeOperation::Task)});
            ExpectError(instance->Replace(replacement.Plan()), AIErrors::DecisionAssetSchemaIncompatible);
            CHECK(fixture.Evaluate(*instance, 2) == Running);
            CHECK(fixture.probe->aborts[1] == 0);
            CHECK(fixture.probe->starts[1] == 1);
        }

        TEST_CASE("Steady-state traversal restart cancellation and looping allocate no core storage", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            auto loop = Node(2, Loop, {4});
            loop.iterations = 3;
            Fixture fixture({Node(1, Parallel, {2, 3}), loop, Node(3, Task), Node(4, Task)},
                            {{Id(5), Id(1), BehaviorTreeServiceMode::Reactive, 1}});
            fixture.probe->outcome[3] = AiTaskState::Running;
            auto instance = fixture.Instance();
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto before = Tests::AllocationProbe::Count();
            auto first = instance->Evaluate(1, snapshot.Value(), fixture.agent);
            auto second = instance->Evaluate(2, snapshot.Value(), fixture.agent);
            auto third = instance->Evaluate(3, snapshot.Value(), fixture.agent);
            auto restarted = instance->Restart();
            auto fourth = instance->Evaluate(4, snapshot.Value(), fixture.agent);
            instance->Shutdown();
            const auto after = Tests::AllocationProbe::Count();
            CHECK(after == before);
            CHECK(first.HasValue());
            CHECK(second.HasValue());
            CHECK(third.HasValue());
            CHECK(fourth.HasValue());
            CHECK(restarted.HasValue());
        }

        TEST_CASE("Replacement storage failure rolls back without aborting the active generation", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            using enum AiTaskState;
            Fixture fixture({Node(1, Sequence, {2}), Node(2, Task)});
            fixture.probe->outcome[2] = Running;
            auto instance = fixture.Instance();
            REQUIRE(fixture.Evaluate(*instance, 1) == Running);
            const auto handle = fixture.probe->handles[2];
            fixture.nodes[0].operation = Selector;
            auto candidate = fixture.Plan();
            auto replaced = [&] {
                Tests::AllocationProbe::ScopedFailure failure;
                return instance->Replace(candidate);
            }();
            ExpectError(replaced, AIErrors::BehaviorTreeStorageUnavailable);
            CHECK(fixture.probe->aborts[2] == 0);
            CHECK(fixture.Evaluate(*instance, 2) == Running);
            CHECK(fixture.probe->handles[2] == handle);
        }

        TEST_CASE("Detached cyclic controls invalid attachments and illegal priority observers never publish a plan",
                  "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            Fixture cyclic({Node(1, Task), Node(2, Sequence, {3}), Node(3, Sequence, {2})});
            ExpectError(BehaviorTreeExecutionPlan::Compile(cyclic.Bindings(), Id(1), cyclic.nodes), AIErrors::BehaviorTreeCycle);
            auto check = Node(1, BlackboardCheck, {2});
            check.abort = BehaviorTreeAbortMode::Both;
            Fixture illegal({check, Node(2, Task)});
            ExpectError(BehaviorTreeExecutionPlan::Compile(illegal.Bindings(), Id(1), illegal.nodes),
                        AIErrors::BehaviorTreeTopologyInvalid);
            Fixture invalidService({Node(1, Task)}, {{Id(2), Id(99), BehaviorTreeServiceMode::Reactive, 1}});
            auto bindings = invalidService.Bindings();
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalidService.nodes, invalidService.services),
                        AIErrors::BehaviorTreeTopologyInvalid);
            invalidService.services[0].intervalTicks = 0;
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalidService.nodes, invalidService.services),
                        AIErrors::BehaviorTreeSchemaInvalid);
        }

        TEST_CASE("Execution compiler rejects malformed enums cardinality identities topology and limits", "[unit][ai][bt_runtime]") {
            using enum BehaviorTreeOperation;
            Fixture fixture({Node(1, Sequence, {2}), Node(2, Task)});
            auto bindings = fixture.Bindings();
            auto invalid = fixture.nodes;
            invalid[0].operation = Count;
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeSchemaInvalid);
            invalid = fixture.nodes;
            invalid[0].children = {Id(999)};
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeTopologyInvalid);
            invalid = fixture.nodes;
            invalid[0].children = {Id(2), Id(2)};
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeTopologyInvalid);
            invalid = fixture.nodes;
            invalid[0].children.clear();
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeSchemaInvalid);
            invalid = fixture.nodes;
            invalid[1].id = Id(1);
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeIdentityConflict);
            invalid = fixture.nodes;
            invalid[1].abort = BehaviorTreeAbortMode::Self;
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeSchemaInvalid);
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(999), fixture.nodes), AIErrors::BehaviorTreeTopologyInvalid);
            ExpectError(BehaviorTreeExecutionPlan::Compile(fixture.Bindings(DecisionPlanKind::StateMachine), Id(1), fixture.nodes),
                        AIErrors::BehaviorTreeSchemaInvalid);
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), fixture.nodes, {}, {.maximumNodes = 1}),
                        AIErrors::BehaviorTreeLimitExceeded);
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), fixture.nodes, {}, {.maximumDepth = 1025}),
                        AIErrors::BehaviorTreeLimitExceeded);
        }
    }  // namespace
}  // namespace Horo::AI
