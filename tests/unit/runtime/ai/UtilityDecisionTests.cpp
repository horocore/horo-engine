#include "DecisionGraphCompilerTestSupport.h"

#include <algorithm>
#include <limits>

namespace Horo::AI {
    namespace {
        using namespace DecisionGraphTestSupport;

        struct UtilityFixture final : Fixture {
            UtilityFixture() {
                const std::array keys{BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(2),
                                                              .kind = BlackboardValueKind::Scalar,
                                                              .defaultValue = BlackboardValue{BlackboardScalarValue{0.25}}}};
                auto captured = BlackboardSchema::Capture({SchemaId(), 1, BlackboardUnknownValuePolicy::Reject, keys});
                REQUIRE(captured.HasValue());
                schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
                auto created = BlackboardInstance::Create(binding, schema);
                REQUIRE(created.HasValue());
                blackboard = std::move(created).Value();
            }

            AiTaskState Tick(DecisionGraphInstance &instance, const std::uint64_t tick, AiWorkBudget &agent, AiWorkBudget &global,
                             DecisionStepTrace *trace = nullptr) {
                auto snapshot = blackboard->Snapshot();
                REQUIRE(snapshot.HasValue());
                const DecisionGraphStepInput input{tick, binding, snapshot.Value(), {}, &agent, &global};
                const auto result = trace ? instance.StepWithTrace(input, *trace) : instance.Step(input);
                REQUIRE(result.HasValue());
                return result.Value();
            }
        };

        DecisionGraphSource UtilitySource(const std::uint64_t graph = 1) {
            UtilityDecisionSource body;
            body.root = Node(1);
            body.requirements = {{.key = MakeIdentity<BlackboardKeyId>(2), .kind = BlackboardValueKind::Scalar, .source = Location()}};
            for (const auto id : {2ULL, 3ULL}) {
                UtilityDecisionOption option;
                option.option = {Node(id),
                                 MakeIdentity<TaskId>(5000 + id),
                                 1,
                                 0,
                                 {{.key = MakeIdentity<BlackboardKeyId>(2), .invert = id == 3}}};
                option.provider = {.id = Node(id), .type = Type(501), .descriptorVersion = {1, 1}, .source = Location()};
                body.options.push_back(std::move(option));
            }
            return {Graph(graph), 1, SchemaId(), {1, 1}, Location(), {}, std::move(body)};
        }

        TEST_CASE("Utility graph source and cooked codecs preserve executable selection and canonical identity",
                  "[ai][decision-compiler][utility]") {
            UtilityFixture fixture;
            auto source = UtilitySource();
            auto encoded = DecisionPlanCodec::EncodeSource(std::span{&source, 1}, Graph(1));
            REQUIRE(encoded.HasValue());
            std::ranges::reverse(std::get<UtilityDecisionSource>(source.body).options);
            const auto reordered = DecisionPlanCodec::EncodeSource(std::span{&source, 1}, Graph(1));
            REQUIRE(reordered.HasValue());
            CHECK(encoded.Value() == reordered.Value());
            DecisionGraphAssetId root;
            const auto decoded = DecisionPlanCodec::DecodeSource(encoded.Value(), root);
            REQUIRE(decoded.HasValue());
            const auto plan = fixture.Compile(decoded.Value(), root.Value());
            auto cooked = DecisionPlanCodec::EncodeCooked(*plan);
            REQUIRE(cooked.HasValue());
            auto loaded = DecisionPlanCodec::DecodeCooked(cooked.Value(), fixture.Environment());
            REQUIRE(loaded.HasValue());
            REQUIRE(loaded.Value().plan);
            CHECK(loaded.Value().plan->Digest() == plan->Digest());
            CHECK(plan->Storage().taskSlots == 2);
            auto instance = fixture.Instance(loaded.Value().plan);
            AiWorkBudget agent{8}, global{8};
            CHECK(fixture.Tick(*instance, 1, agent, global) == AiTaskState::Running);
            REQUIRE(fixture.probe.calls.size() == 1);
            CHECK(fixture.probe.calls.front() == "start:5003");
            CHECK(fixture.Tick(*instance, 2, agent, global) == AiTaskState::Succeeded);
            CHECK(fixture.probe.starts == 1);
            CHECK(fixture.probe.resumes == 1);
            CHECK(fixture.probe.cleanups == 1);
        }

        TEST_CASE("Utility graph reserves host budgets before selection and emits bounded versioned score evidence",
                  "[ai][decision-compiler][utility]") {
            UtilityFixture fixture;
            auto source = UtilitySource();
            auto instance = fixture.Instance(fixture.Compile(std::span{&source, 1}, 1));
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            CHECK(instance->Step({1, fixture.binding, snapshot.Value(), {}}).HasError());
            AiWorkBudget shortAgent{3}, global{12};
            DecisionStepTrace trace;
            CHECK(fixture.Tick(*instance, 1, shortAgent, global, &trace) == AiTaskState::Running);
            CHECK(shortAgent.Remaining() == 3);
            CHECK(global.Remaining() == 12);
            CHECK(fixture.probe.starts == 0);
            CHECK(trace.Records().empty());
            AiWorkBudget agent{4};
            CHECK(fixture.Tick(*instance, 2, agent, global, &trace) == AiTaskState::Succeeded);
            CHECK(agent.Remaining() == 0);
            CHECK(global.Remaining() == 8);
            CHECK(trace.Header().version == 2);
            CHECK(trace.Header().blackboardRevision == snapshot.Value().CapturedRevision());
            CHECK(std::ranges::count(trace.Records(), DecisionStepRecordKind::UtilityScore, &DecisionStepRecord::kind) == 4);
            CHECK(trace.Records().size() == 5);
            CHECK(trace.Records()[0].key == MakeIdentity<BlackboardKeyId>(2));
            CHECK(trace.Records()[0].utilityScore == 0.25);
            CHECK_FALSE(trace.Records()[1].key.IsValid());
        }

        TEST_CASE("Utility graph cancellation and repeated mixed-family calls use canonical task cleanup and generations",
                  "[ai][decision-compiler][utility]") {
            UtilityFixture fixture;
            const std::array sources{ParallelCalls(1, 2, DecisionPlanKind::Utility), UtilitySource(2)};
            const auto plan = fixture.Compile(sources, 1);
            auto instance = fixture.Instance(plan);
            AiWorkBudget agent{8}, global{8};
            CHECK(fixture.Tick(*instance, 1, agent, global) == AiTaskState::Running);
            REQUIRE(fixture.probe.tasks.size() == 2);
            CHECK(fixture.probe.tasks[0] != fixture.probe.tasks[1]);
            REQUIRE(instance->Cancel(AiTaskCancellationReason::Requested).HasValue());
            CHECK(fixture.probe.aborts == 2);
            CHECK(fixture.probe.cleanups == 2);
            REQUIRE(instance->Cancel(AiTaskCancellationReason::Requested).HasValue());
            CHECK(fixture.probe.aborts == 2);
            CHECK(fixture.probe.cleanups == 2);
        }

        TEST_CASE("HSM Utility reentry advances the selected task generation and releases prior callbacks exactly once",
                  "[ai][decision-compiler][utility]") {
            UtilityFixture fixture;
            fixture.probe.succeedTick = 100;
            auto machine = CallingMachine(1, 2, DecisionPlanKind::Utility);
            auto &body = std::get<StateMachineAssetDescriptor>(machine.body);
            body.states.resize(1);
            body.transitions[0].target = Node(1);
            body.transitions[0].childOutcome.reset();
            body.transitions[0].event = MakeIdentity<StateMachineEventId>(77);
            const std::array sources{machine, UtilitySource(2)};
            auto instance = fixture.Instance(fixture.Compile(sources, 1));
            AiWorkBudget agent{20}, global{20};
            CHECK(fixture.Tick(*instance, 1, agent, global) == AiTaskState::Running);
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const std::array events{MakeIdentity<StateMachineEventId>(77)};
            REQUIRE(instance->Step({2, fixture.binding, snapshot.Value(), events, &agent, &global}).HasValue());
            CHECK(fixture.Tick(*instance, 3, agent, global) == AiTaskState::Running);
            REQUIRE(fixture.probe.tasks.size() == 2);
            CHECK(fixture.probe.tasks[0].slot.index == fixture.probe.tasks[1].slot.index);
            CHECK(fixture.probe.tasks[0].slot.generation < fixture.probe.tasks[1].slot.generation);
            CHECK(fixture.probe.aborts == 1);
            CHECK(fixture.probe.cleanups == 1);
            REQUIRE(instance->Cancel(AiTaskCancellationReason::Requested).HasValue());
            CHECK(fixture.probe.aborts == 2);
            CHECK(fixture.probe.cleanups == 2);
        }

        TEST_CASE("Utility compilation rejects missing scalar bindings malformed providers and nonfinite policy before factories",
                  "[ai][decision-compiler][utility]") {
            UtilityFixture fixture;
            auto source = UtilitySource();
            auto &body = std::get<UtilityDecisionSource>(source.body);
            SECTION("Required scalar binding is absent") {
                body.requirements.clear();
            }
            SECTION("Option has a conflicting provider identity") {
                body.options[0].provider.id = Node(90);
            }
            SECTION("A task provider is required") {
                body.options[0].provider.type = Type(999);
            }
            SECTION("Untrusted nonfinite score policy is rejected") {
                body.options[0].option.priority = std::numeric_limits<double>::infinity();
            }
            const auto compiled = DecisionGraphCompiler::Compile(Graph(1), std::span{&source, 1}, fixture.Environment());
            CHECK((compiled.HasError() || !compiled.Value().plan));
            CHECK(fixture.probe.factories == 0);
        }
    }  // namespace
}  // namespace Horo::AI
