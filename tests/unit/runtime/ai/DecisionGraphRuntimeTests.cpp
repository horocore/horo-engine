#include "DecisionGraphCompilerTestSupport.h"

#include <thread>

namespace Horo::AI {
    namespace {
        using namespace DecisionGraphTestSupport;

        TEST_CASE("BT to HSM to BT executes real independent parallel call occurrences across committed ticks",
                  "[ai][decision-graph-runtime]") {
            Fixture fixture;
            const std::array sources{ParallelCalls(1, 2, DecisionPlanKind::StateMachine),
                                     CallingMachine(2, 3, DecisionPlanKind::BehaviorTree), TaskTree(3)};
            auto plan = fixture.Compile(sources, 1);
            auto instance = fixture.Instance(plan);
            CHECK(fixture.probe.factories == 2);
            CHECK(fixture.Step(*instance, 1) == AiTaskState::Running);
            CHECK(fixture.probe.starts == 2);
            REQUIRE(fixture.probe.tasks.size() == 2);
            CHECK(fixture.probe.tasks[0] != fixture.probe.tasks[1]);
            CHECK(fixture.Step(*instance, 2) == AiTaskState::Succeeded);
            CHECK(fixture.probe.resumes == 2);
            CHECK(fixture.probe.cleanups == 2);
            CHECK(fixture.probe.aborts == 0);
            CHECK(fixture.Step(*instance, 3) == AiTaskState::Succeeded);
            CHECK(fixture.probe.resumes == 2);
            CHECK(fixture.probe.cleanups == 2);
        }

        TEST_CASE("A pure terminal HSM call needs no native provider and preserves its authored result", "[ai][decision-graph-runtime]") {
            Fixture fixture;
            const std::array sources{CallTree(1, 2, DecisionPlanKind::StateMachine), TerminalMachine(2)};
            auto plan = fixture.Compile(sources, 1);
            CHECK(plan->Graphs()[0].localStorage.taskSlots == 0);
            CHECK(plan->Storage().nativeDeclaredProviders == 0);
            auto instance = fixture.Instance(plan);
            CHECK(fixture.probe.factories == 0);
            CHECK(fixture.Step(*instance, 1) == AiTaskState::Succeeded);
            CHECK(fixture.Step(*instance, 2) == AiTaskState::Succeeded);
            CHECK(fixture.probe.starts == 0);
        }

        TEST_CASE("HSM to BT cancellation preserves exactly once child abort cleanup without new state actions",
                  "[ai][decision-graph-runtime]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            const std::array sources{CallingMachine(1, 2, DecisionPlanKind::BehaviorTree), TaskTree(2)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            REQUIRE(fixture.Step(*instance, 1) == AiTaskState::Running);
            REQUIRE(instance->Cancel(AiTaskCancellationReason::Requested).HasValue());
            CHECK(fixture.probe.starts == 1);
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
            REQUIRE(instance->Cancel(AiTaskCancellationReason::Requested).HasValue());
            instance.reset();
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
        }

        TEST_CASE("Decision graph provider retirement fences all later callbacks while releasing actual live child tasks",
                  "[ai][decision-graph-runtime]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            const std::array sources{CallTree(1, 2, DecisionPlanKind::StateMachine), CallingMachine(2, 3, DecisionPlanKind::BehaviorTree),
                                     TaskTree(3)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            REQUIRE(fixture.Step(*instance, 1) == AiTaskState::Running);
            fixture.admission->store(false, std::memory_order_release);
            CHECK(fixture.Step(*instance, 2) == AiTaskState::Cancelled);
            CHECK(fixture.probe.resumes == 0);
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
        }

        TEST_CASE("Decision graph rejects reentrant off owner and stale blackboard observations before execution",
                  "[ai][decision-graph-runtime]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            bool reentrantRejected{};
            fixture.probe.onStart = [&] {
                reentrantRejected = instance->Step({2, fixture.binding, snapshot.Value(), {}}).HasError();
            };
            CHECK(fixture.Step(*instance, 1) == AiTaskState::Running);
            CHECK(reentrantRejected);
            bool offOwnerRejected{};
            std::thread observer([&] {
                offOwnerRejected = instance->Step({2, fixture.binding, snapshot.Value(), {}}).HasError();
            });
            observer.join();
            CHECK(offOwnerRejected);
            auto stale = fixture.binding;
            ++stale.instanceGeneration;
            CHECK(instance->Step({2, stale, snapshot.Value(), {}}).HasError());
            CHECK(fixture.probe.resumes == 0);
            CHECK(fixture.Step(*instance, 2) == AiTaskState::Succeeded);
        }

        TEST_CASE("Decision graph requires the actual captured immutable schema lease before executing a task",
                  "[ai][decision-graph-runtime]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            const std::array keys{BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(1),
                                                          .kind = BlackboardValueKind::SignedInteger,
                                                          .defaultValue = BlackboardValue{BlackboardScalarValue{std::int64_t{7}}}}};
            auto capture = BlackboardSchema::Capture({SchemaId(), 1, BlackboardUnknownValuePolicy::Reject, keys});
            REQUIRE(capture.HasValue());
            auto different = std::make_shared<const BlackboardSchema>(std::move(capture).Value());
            auto other = BlackboardInstance::Create(fixture.binding, different);
            REQUIRE(other.HasValue());
            auto snapshot = other.Value()->Snapshot();
            REQUIRE(snapshot.HasValue());
            CHECK(snapshot.Value().Schema() == different);
            CHECK(instance->Step({1, fixture.binding, snapshot.Value(), {}}).HasError());
            CHECK(fixture.probe.starts == 0);
            CHECK(fixture.Step(*instance, 1) == AiTaskState::Running);
        }

        TEST_CASE("HSM one-shot provider actions terminate or reject Running with exactly once cleanup", "[ai][decision-graph-runtime]") {
            Fixture fixture;
            auto source = TerminalMachine(1);
            auto &state = std::get<StateMachineAssetDescriptor>(source.body).states[0];
            state.entry = MakeIdentity<TaskId>(6001);
            state.entryProvider = StateMachineProviderAction{{.id = Node(10), .type = Type(501), .source = Location()}, {}};
            const std::array sources{source};
            SECTION("actual terminal success") {
                fixture.probe.succeedTick = 1;
                auto instance = fixture.Instance(fixture.Compile(sources, 1));
                CHECK(fixture.Step(*instance, 1) == AiTaskState::Succeeded);
                CHECK(fixture.probe.starts == 1);
                CHECK(fixture.probe.aborts == 0);
                CHECK(fixture.probe.cleanups == 1);
            }
            SECTION("Running cannot escape as unowned intent") {
                auto instance = fixture.Instance(fixture.Compile(sources, 1));
                auto snapshot = fixture.blackboard->Snapshot();
                REQUIRE(snapshot.HasValue());
                CHECK(instance->Step({1, fixture.binding, snapshot.Value(), {}}).HasError());
                CHECK(fixture.probe.starts == 1);
                CHECK(fixture.probe.aborts == 1);
                CHECK(fixture.probe.cleanups == 1);
            }
        }

        TEST_CASE("HSM normal exit cancels a running child before invoking its actual exit provider", "[ai][decision-graph-runtime]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            const std::array registrations{Registration(fixture.probe), Registration(fixture.probe, 502, true)};
            auto captured = DecisionNodeGeneration::Capture(registrations, fixture.admission);
            REQUIRE(captured.HasValue());
            fixture.providers = std::move(captured).Value();
            auto machine = CallingMachine(1, 2, DecisionPlanKind::BehaviorTree);
            auto &body = std::get<StateMachineAssetDescriptor>(machine.body);
            body.states[0].exit = MakeIdentity<TaskId>(6001);
            body.states[0].exitProvider = StateMachineProviderAction{{.id = Node(10), .type = Type(502), .source = Location()}, {}};
            body.transitions[0].childOutcome.reset();
            body.transitions[0].event = MakeIdentity<StateMachineEventId>(77);
            const std::array sources{machine, TaskTree(2)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            REQUIRE(fixture.Step(*instance, 1) == AiTaskState::Running);
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const std::array events{MakeIdentity<StateMachineEventId>(77)};
            auto exited = instance->Step({2, fixture.binding, snapshot.Value(), events});
            REQUIRE(exited.HasValue());
            CHECK(exited.Value() == AiTaskState::Succeeded);
            const std::vector<std::string> expected{"start:5002", "abort", "cleanup", "start:6001", "cleanup"};
            CHECK(fixture.probe.calls == expected);
        }

        TEST_CASE("Hierarchical HSM call exits cancel actual descendant occurrences before ancestor occurrences",
                  "[ai][decision-graph-runtime]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            const DecisionSubplanCall call{Graph(2), DecisionPlanKind::BehaviorTree, {1, 1}};
            StateMachineAssetDescriptor body{.initial = Node(1),
                                             .states = {{.id = Node(1), .initialChild = Node(2), .call = call},
                                                        {.id = Node(2), .parent = Node(1), .call = call},
                                                        {.id = Node(3), .terminal = DecisionStateTerminal::Succeeded}},
                                             .transitions = {{.id = Node(10),
                                                              .source = Node(2),
                                                              .target = Node(3),
                                                              .event = MakeIdentity<StateMachineEventId>(77)}}};
            DecisionGraphSource machine{Graph(1), 1, SchemaId(), {1, 1}, Location(), MakeIdentity<TaskId>(10001), body};
            const std::array sources{machine, TaskTree(2)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            REQUIRE(fixture.Step(*instance, 1) == AiTaskState::Running);
            REQUIRE(fixture.probe.tasks.size() == 2);
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const std::array events{MakeIdentity<StateMachineEventId>(77)};
            auto completed = instance->Step({2, fixture.binding, snapshot.Value(), events});
            REQUIRE(completed.HasValue());
            CHECK(completed.Value() == AiTaskState::Succeeded);
            REQUIRE(fixture.probe.abortedTasks.size() == 2);
            CHECK(fixture.probe.abortedTasks[0] == fixture.probe.tasks[1]);
            CHECK(fixture.probe.abortedTasks[1] == fixture.probe.tasks[0]);
            CHECK(fixture.probe.cleanups == 2);
        }

        TEST_CASE("HSM self reentry resets only the child occurrence and advances its real task generation",
                  "[ai][decision-graph-runtime]") {
            Fixture fixture;
            fixture.probe.succeedTick = 100;
            auto machine = CallingMachine(1, 2, DecisionPlanKind::BehaviorTree);
            auto &body = std::get<StateMachineAssetDescriptor>(machine.body);
            body.states.resize(1);
            body.transitions[0].target = Node(1);
            body.transitions[0].childOutcome.reset();
            body.transitions[0].event = MakeIdentity<StateMachineEventId>(77);
            const std::array sources{machine, TaskTree(2)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            REQUIRE(fixture.Step(*instance, 1) == AiTaskState::Running);
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const std::array events{MakeIdentity<StateMachineEventId>(77)};
            auto reentered = instance->Step({2, fixture.binding, snapshot.Value(), events});
            REQUIRE(reentered.HasValue());
            REQUIRE(fixture.Step(*instance, 3) == AiTaskState::Running);
            REQUIRE(fixture.probe.tasks.size() == 2);
            CHECK(fixture.probe.tasks[0].slot.index == fixture.probe.tasks[1].slot.index);
            CHECK(fixture.probe.tasks[0].slot.generation < fixture.probe.tasks[1].slot.generation);
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
            REQUIRE(instance->Cancel(AiTaskCancellationReason::Requested).HasValue());
            CHECK(fixture.probe.aborts == 2);
            CHECK(fixture.probe.cleanups == 2);
        }

        TEST_CASE("Decision graph reserves the entire compiled owner task range before creating callbacks",
                  "[ai][decision-graph-runtime]") {
            Fixture fixture;
            const std::array sources{ParallelCalls(1, 2, DecisionPlanKind::BehaviorTree), TaskTree(2)};
            auto plan = fixture.Compile(sources, 1);
            auto binding = fixture.RuntimeBinding();
            binding.availableTaskSlots = plan->Storage().taskSlots - 1;
            CHECK(DecisionGraphInstance::Create(plan, fixture.providers, binding).HasError());
            CHECK(fixture.probe.factories == 0);
        }
    }  // namespace
}  // namespace Horo::AI
