#include "DecisionGraphCompilerTestSupport.h"

#include <algorithm>
#include <thread>

namespace Horo::AI {
    namespace {
        using namespace DecisionGraphTestSupport;

        /** @brief Uses descending transition identities to distinguish execution order from registry order. */
        DecisionGraphSource TransitionChain(const std::uint64_t graph) {
            auto source = TerminalMachine(graph);
            StateMachineAssetDescriptor machine{.initial = Node(1)};
            for (std::uint64_t index = 1; index <= StateMachineLimits::TransitionsPerStep + 1; ++index)
                machine.states.push_back({.id = Node(index), .source = Location()});
            machine.states.back().terminal = DecisionStateTerminal::Succeeded;
            for (std::uint64_t index = 0; index < StateMachineLimits::TransitionsPerStep; ++index)
                machine.transitions.push_back(
                    {.id = Node(200 - index), .source = Node(index + 1), .target = Node(index + 2), .location = Location()});
            machine.maximumTransitionsPerStep = StateMachineLimits::TransitionsPerStep;
            source.body = std::move(machine);
            return source;
        }

        /** @brief Replays the same deterministic inputs through an independently owned fixture. */
        void CheckTraceReplay(const std::span<const DecisionGraphSource> sources, const DecisionStepTrace &retained,
                              const std::vector<TaskHandle> &tracedTasks) {
            Fixture replay;
            auto twin = replay.Instance(replay.Compile(sources, 1));
            const auto replayInput = replay.blackboard->Snapshot();
            REQUIRE(replayInput.HasValue());
            DecisionStepTrace replayed;
            REQUIRE(twin->StepWithTrace({1, replay.binding, replayInput.Value(), {}}, replayed).HasValue());
            CHECK(replayed.Header().planDigest == retained.Header().planDigest);
            CHECK(std::ranges::equal(replayed.Records(), retained.Records()));
            CHECK(replay.probe.tasks == tracedTasks);
        }

        TEST_CASE("HSM traces retain the complete committed chain and replay its execution order", "[ai][decision-graph-runtime][trace]") {
            Fixture fixture;
            const std::array sources{TransitionChain(1)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            const auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            DecisionStepTrace trace;
            REQUIRE(instance->StepWithTrace({1, fixture.binding, snapshot.Value(), {}}, trace).Value() == AiTaskState::Succeeded);
            REQUIRE(trace.Records().size() == StateMachineLimits::TransitionsPerStep + 1);
            for (std::uint32_t index = 0; index < StateMachineLimits::TransitionsPerStep; ++index) {
                const auto &record = trace.Records()[index];
                CHECK(record.kind == DecisionStepRecordKind::StateMachineTransition);
                CHECK(record.node == Node(200 - index));
                CHECK(record.transitionCount == index + 1);
                CHECK(record.occurrence.graph == Graph(1));
            }
            CHECK(trace.DroppedRecords() == 0);
            CheckTraceReplay(sources, trace, {});
        }

        TEST_CASE("HSM chain trace overflow preserves all independent call outcomes", "[ai][decision-graph-runtime][trace]") {
            Fixture fixture;
            const std::array sources{ParallelCalls(1, 2, DecisionPlanKind::BehaviorTree),
                                     ParallelCalls(2, 3, DecisionPlanKind::BehaviorTree),
                                     ParallelCalls(3, 4, DecisionPlanKind::BehaviorTree),
                                     ParallelCalls(4, 5, DecisionPlanKind::StateMachine), TransitionChain(5)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            const auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            DecisionStepTrace trace;
            REQUIRE(instance->StepWithTrace({1, fixture.binding, snapshot.Value(), {}}, trace).Value() == AiTaskState::Succeeded);
            CHECK(trace.Records().size() == DecisionStepTrace::MaximumRecords);
            CHECK(trace.DroppedRecords() > 0);
            Fixture untraced;
            auto twin = untraced.Instance(untraced.Compile(sources, 1));
            CHECK(untraced.Step(*twin, 1) == AiTaskState::Succeeded);
            CHECK(fixture.probe.factories == 0);
        }

        TEST_CASE("HSM traces retain committed evidence when a later entry provider fails", "[ai][decision-graph-runtime][trace]") {
            Fixture fixture;
            auto source = TransitionChain(1);
            auto &state = std::get<StateMachineAssetDescriptor>(source.body).states[2];
            state.entry = MakeIdentity<TaskId>(6001);
            state.entryProvider = StateMachineProviderAction{{.id = Node(500), .type = Type(501), .source = Location()}, {}};
            const std::array sources{source};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            const auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            DecisionStepTrace trace;
            REQUIRE(instance->StepWithTrace({1, fixture.binding, snapshot.Value(), {}}, trace).HasError());
            REQUIRE(trace.Records().size() == 1);
            CHECK(trace.Records()[0].kind == DecisionStepRecordKind::StateMachineTransition);
            CHECK(trace.Records()[0].node == Node(200));
            CHECK(trace.Header().outcome == AiTaskState::Failed);
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
        }

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

        TEST_CASE("Traced graph steps preserve independent task order and report actual HSM transitions",
                  "[ai][decision-graph-runtime][trace]") {
            Fixture fixture;
            const std::array sources{ParallelCalls(1, 2, DecisionPlanKind::StateMachine),
                                     CallingMachine(2, 3, DecisionPlanKind::BehaviorTree), TaskTree(3)};
            const auto plan = fixture.Compile(sources, 1);
            auto instance = fixture.Instance(plan);
            const auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            DecisionStepTrace trace;
            const auto first = instance->StepWithTrace({1, fixture.binding, snapshot.Value(), {}}, trace);
            REQUIRE(first.HasValue());
            CHECK(first.Value() == AiTaskState::Running);
            CHECK(trace.Header().version == 3);
            CHECK(trace.Header().tick == 1);
            CHECK(trace.Header().root == plan->Root());
            CHECK(trace.Header().planDigest == plan->Digest());
            CHECK(trace.Header().blackboardRevision == snapshot.Value().Revision().Value());
            CHECK(trace.Header().outcome == first.Value());
            CHECK(trace.DroppedRecords() == 0);
            std::vector<TaskHandle> tracedTasks;
            for (const auto &record : trace.Records()) {
                if (record.occurrence.graph != Graph(3) || !record.handle.IsValid())
                    continue;
                CHECK(record.node == Node(2));
                CHECK(record.state == AiTaskState::Running);
                tracedTasks.push_back(record.handle);
            }
            REQUIRE(tracedTasks.size() == 2);
            CHECK(tracedTasks == fixture.probe.tasks);
            CHECK(tracedTasks[0] != tracedTasks[1]);

            const auto retained = trace;
            const auto second = instance->StepWithTrace({2, fixture.binding, snapshot.Value(), {}}, trace);
            REQUIRE(second.HasValue());
            CHECK(second.Value() == AiTaskState::Succeeded);
            CHECK(std::ranges::count_if(trace.Records(), [](const DecisionStepRecord &record) {
                return record.kind == DecisionStepRecordKind::StateMachineStep && record.lastTransition == Node(3) &&
                       record.transitionCount == 1;
            }) == 2);
            CHECK(retained.Header().tick == 1);
            CHECK(instance->StepWithTrace({2, fixture.binding, snapshot.Value(), {}}, trace).HasError());
            CHECK(trace.Header().tick == 2);

            CheckTraceReplay(sources, retained, tracedTasks);
        }

        TEST_CASE("Decision trace truncation leaves graph work and task admission unchanged", "[ai][decision-graph-runtime][trace]") {
            Fixture fixture;
            std::vector<DecisionGraphSource> sources;
            for (std::uint64_t graph = 1; graph < 6; ++graph)
                sources.push_back(ParallelCalls(graph, graph + 1, DecisionPlanKind::BehaviorTree));
            sources.push_back(TaskTree(6));
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            const auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            DecisionStepTrace trace;
            REQUIRE(instance->StepWithTrace({1, fixture.binding, snapshot.Value(), {}}, trace).Value() == AiTaskState::Running);
            CHECK(trace.Records().size() == DecisionStepTrace::MaximumRecords);
            CHECK(trace.DroppedRecords() > 0);
            CHECK(fixture.probe.starts == 32);
            CHECK(fixture.Step(*instance, 2) == AiTaskState::Succeeded);
            CHECK(fixture.probe.resumes == 32);
            CHECK(fixture.probe.cleanups == 32);
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

        TEST_CASE("Decision trace rejects invalid observation admission without rewriting retained evidence",
                  "[ai][decision-graph-runtime][trace]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            DecisionStepTrace trace;
            REQUIRE(instance->StepWithTrace({1, fixture.binding, snapshot.Value(), {}}, trace).HasValue());
            const auto retained = trace;
            auto current = fixture.binding;
            SECTION("cancelled operation still requires exact owner metadata") {
                fixture.cancellation.RequestCancellation();
                ++current.agent.slot.index;
            }
            SECTION("live operation rejects a revoked read lease before trace publication") {
                REQUIRE(fixture.blackboard->TeardownAtBlackboardSync().HasValue());
            }
            const auto rejected = instance->StepWithTrace({2, current, snapshot.Value(), {}}, trace);
            TestSupport::ExpectError(rejected, AIErrors::BlackboardInstanceStale);
            CHECK(trace.Header().agent == retained.Header().agent);
            CHECK(trace.Header().tick == retained.Header().tick);
            CHECK(trace.Header().blackboardRevision == retained.Header().blackboardRevision);
            CHECK(std::ranges::equal(trace.Records(), retained.Records()));
            CHECK(fixture.probe.resumes == 0);
            CHECK(fixture.probe.aborts == 0);
            CHECK(fixture.probe.cleanups == 0);
            fixture.cancellation.RequestCancellation();
            auto cancelled = instance->StepWithTrace({2, fixture.binding, snapshot.Value(), {}}, trace);
            REQUIRE(cancelled.HasValue());
            CHECK(cancelled.Value() == AiTaskState::Cancelled);
            CHECK(trace.Header().agent == fixture.binding.agent);
            CHECK(trace.Header().tick == 2);
            CHECK(trace.Header().blackboardRevision == snapshot.Value().CapturedRevision());
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
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
