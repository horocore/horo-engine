#include "AiTestSupport.h"
#include "Horo/AI/StateMachine.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;
        static_assert(!std::is_default_constructible_v<CookedStateMachinePlan::ConstructionToken>);
        static_assert(!std::is_default_constructible_v<StateMachineInstance::ConstructionToken>);

        DecisionNodeId Node(const std::uint64_t value) {
            return MakeIdentity<DecisionNodeId>(value);
        }

        struct MachineFixture final {
            std::shared_ptr<const BlackboardSchema> schema;
            BlackboardInstanceBinding binding;
            std::unique_ptr<BlackboardInstance> blackboard;
            CancellationSource cancellation;

            MachineFixture() {
                const std::array keys{BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(80),
                                                              .kind = BlackboardValueKind::SignedInteger,
                                                              .defaultValue = BlackboardValue{BlackboardScalarValue{std::int64_t{5}}}},
                                      BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(81),
                                                              .presence = BlackboardKeyPresence::Optional},
                                      BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(82),
                                                              .kind = BlackboardValueKind::Scalar,
                                                              .defaultValue = BlackboardValue{BlackboardScalarValue{2.5}}}};
                auto captured =
                    BlackboardSchema::Capture({MakeIdentity<BlackboardSchemaId>(70), 1, BlackboardUnknownValuePolicy::Reject, keys});
                REQUIRE(captured.HasValue());
                schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
                binding = {{AiRuntimeIncarnation::Create(77).Value(), {3, 4}}, schema->Identity(), 1, 1, 1};
                auto created = BlackboardInstance::Create(binding, schema);
                REQUIRE(created.HasValue());
                blackboard = std::move(created).Value();
            }

            std::shared_ptr<const DecisionAssetPlan> Decision(const StateMachineAssetDescriptor &source, const bool bindGuards = true,
                                                              const DecisionPlanKind kind = DecisionPlanKind::StateMachine) const {
                const auto type = MakeIdentity<DecisionNodeTypeId>(40);
                DecisionAssetDescriptor asset{.asset = MakeIdentity<DecisionGraphAssetId>(50),
                                              .kind = kind,
                                              .blackboardSchema = schema->Identity(),
                                              .source = {"machine.horo_sm", 1, 1}};
                for (const auto &state : source.states)
                    asset.nodes.push_back({.id = state.id, .type = type, .source = asset.source});
                for (const auto &transition : source.transitions) {
                    DecisionAssetNode node{.id = transition.id, .type = type, .source = asset.source};
                    if (bindGuards) {
                        for (const auto &guard : transition.guards) {
                            const auto key = std::ranges::find(schema->Keys(), guard.key, &BlackboardKeyDescriptor::key);
                            REQUIRE(key != schema->Keys().end());
                            node.requirements.push_back({.key = key->key, .kind = key->kind, .source = asset.source});
                        }
                    }
                    asset.nodes.push_back(std::move(node));
                }
                const std::array descriptors{
                    DecisionNodeDescriptor{.type = type,
                                           .origin = {DecisionDescriptorSourceKind::Native, MakeIdentity<DecisionProviderId>(60), 1},
                                           .source = asset.source}};
                const std::array schemas{schema};
                auto compiled = DecisionAssetCompiler::Compile(asset, {}, descriptors, schemas);
                REQUIRE(compiled.HasValue());
                REQUIRE(compiled.Value().IsValid());
                return compiled.Value().plan;
            }

            std::unique_ptr<StateMachineInstance> Instance(const StateMachineAssetDescriptor &source) const {
                auto plan = CookedStateMachinePlan::Compile(Decision(source), source);
                REQUIRE(plan.HasValue());
                auto instance = StateMachineInstance::Create(plan.Value(), binding,
                                                             {MakeIdentity<TaskId>(90),
                                                              {binding.agent.incarnation, {8, 1}},
                                                              binding.agent,
                                                              cancellation.Token()});
                REQUIRE(instance.HasValue());
                return std::move(instance).Value();
            }

            BlackboardSnapshot Snapshot() const {
                auto snapshot = blackboard->Snapshot();
                REQUIRE(snapshot.HasValue());
                return std::move(snapshot).Value();
            }
        };

        StateMachineAssetDescriptor Flat() {
            return {.initial = Node(1),
                    .states = {{.id = Node(1)}, {.id = Node(2)}, {.id = Node(3)}},
                    .transitions = {{.id = Node(10), .source = Node(1), .target = Node(2)},
                                    {.id = Node(11), .source = Node(2), .target = Node(3)}}};
        }

        struct ActionLog final : StateMachineActions {
            struct Fact {
                DecisionNodeId state;
                StateMachineActionPhase phase;
            };

            std::array<Fact, 64> facts{};
            std::size_t count{};
            DecisionNodeId failState;
            StateMachineActionPhase failPhase{StateMachineActionPhase::Entry};
            CancellationSource *cancel{};
            BlackboardInstance *retire{};
            StateMachineInstance *runner{};
            bool reentrantRejected{};
            bool stepRejected{};

            Result<void> Invoke(const StateMachineActionInvocation &call) noexcept override {
                auto &log = *this;
                log.facts[log.count++] = {call.state, call.phase};
                if (log.runner != nullptr) {
                    log.reentrantRejected = log.runner->Cancel(AiTaskCancellationReason::Requested).HasError();
                    log.stepRejected = log.runner->Step(call.tick + 1, call.blackboard.Binding(), call.blackboard).HasError();
                }
                if (log.cancel != nullptr)
                    log.cancel->RequestCancellation();
                if (log.retire != nullptr)
                    static_cast<void>(log.retire->TeardownAtBlackboardSync());
                if (call.state == log.failState && call.phase == log.failPhase)
                    return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                return Result<void>::Success();
            }

            StateMachineActions *Adapter() {
                return this;
            }
        };

        StateMachineAssetDescriptor Hierarchy() {
            auto source = Flat();
            source.initial = Node(1);
            source.states[0].initialChild = Node(2);
            source.states[1].parent = Node(1);
            source.states[2].parent = Node(1);
            source.transitions = {{.id = Node(10), .source = Node(2), .target = Node(3)}};
            for (auto &state : source.states) {
                state.entry = MakeIdentity<TaskId>(100);
                state.update = MakeIdentity<TaskId>(101);
                state.exit = MakeIdentity<TaskId>(102);
            }
            return source;
        }
    }  // namespace

    TEST_CASE("State-machine default commits one transition per declared step", "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto source = Flat();
        std::ranges::reverse(source.states);
        auto instance = fixture.Instance(source);
        const auto snapshot = fixture.Snapshot();
        const auto first = instance->Step(1, fixture.binding, snapshot);
        REQUIRE(first.HasValue());
        CHECK(first.Value().transitions == 1);
        CHECK(first.Value().activeState == Node(2));
        ExpectError(instance->Step(1, fixture.binding, snapshot), StateMachineErrors::StepInvalid);
        ExpectError(instance->Step(0, fixture.binding, snapshot), StateMachineErrors::StepInvalid);
        const auto second = instance->Step(2, fixture.binding, snapshot);
        REQUIRE(second.HasValue());
        CHECK(second.Value().activeState == Node(3));
        CHECK(instance->Step(3, fixture.binding, snapshot).Value().transitions == 0);
    }

    TEST_CASE("State-machine priority ties use stable identity not authoring order", "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto source = Flat();
        source.transitions[1].source = Node(1);
        source.transitions[1].priority = 7;
        source.transitions[0].priority = 7;
        std::ranges::reverse(source.transitions);
        auto instance = fixture.Instance(source);
        CHECK(instance->Step(1, fixture.binding, fixture.Snapshot()).Value().lastTransition == Node(10));
        source.transitions[0].priority = 8;
        instance = fixture.Instance(source);
        CHECK(instance->Step(1, fixture.binding, fixture.Snapshot()).Value().lastTransition == Node(11));
    }

    TEST_CASE("State-machine guards bind typed frozen gameplay blackboard facts", "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto source = Flat();
        source.transitions[0].guards = {{MakeIdentity<BlackboardKeyId>(80), StateMachineGuardOperator::Greater, std::int64_t{8}}};
        auto instance = fixture.Instance(source);
        const auto frozen = fixture.Snapshot();
        CHECK(instance->Step(1, fixture.binding, frozen).Value().activeState == Node(1));
        auto batch = fixture.blackboard->BeginWriteBatch();
        REQUIRE(batch.HasValue());
        auto writes = std::move(batch).Value();
        REQUIRE(writes.Stage({MakeIdentity<BlackboardKeyId>(80), BlackboardValue{BlackboardScalarValue{std::int64_t{9}}}}).HasValue());
        const auto committed = fixture.blackboard->CommitAtBlackboardSync(std::move(writes));
        REQUIRE(committed.HasValue());
        CHECK(instance->Step(2, fixture.binding, frozen).Value().activeState == Node(1));
        CHECK(instance->Step(3, fixture.binding, fixture.Snapshot()).Value().activeState == Node(2));
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source, false), source), AIErrors::DecisionAssetBindingMissing);
        source.transitions[0].guards[0].operand = true;
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source), source), AIErrors::BlackboardValueTypeMismatch);
    }

    TEST_CASE("State-machine guard operators preserve exact scalar kinds and conjunctions", "[unit][ai][state-machine]") {
        using enum StateMachineGuardOperator;
        MachineFixture fixture;
        for (const auto operation : {Equal, NotEqual, Less, Greater}) {
            auto source = Flat();
            double operand = 2.0;
            if (operation == Equal)
                operand = 2.5;
            else if (operation == Less)
                operand = 3.0;
            source.transitions[0].guards = {{MakeIdentity<BlackboardKeyId>(82), operation, operand},
                                            {MakeIdentity<BlackboardKeyId>(80), Equal, std::int64_t{5}}};
            auto instance = fixture.Instance(source);
            CHECK(instance->Step(1, fixture.binding, fixture.Snapshot()).Value().activeState == Node(2));
            source.transitions[0].guards[1].operand = std::int64_t{7};
            instance = fixture.Instance(source);
            CHECK(instance->Step(1, fixture.binding, fixture.Snapshot()).Value().activeState == Node(1));
        }
        auto source = Flat();
        source.transitions[0].guards = {{MakeIdentity<BlackboardKeyId>(82), Equal, std::numeric_limits<double>::quiet_NaN()}};
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source), source), AIErrors::BlackboardValueInvalid);
        source.transitions[0].guards[0].operation = Count;
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source), source), AIErrors::DecisionAssetSchemaInvalid);
    }

    TEST_CASE("State-machine absent optional guards and step-scoped events cannot fabricate transitions", "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto source = Flat();
        source.transitions[0].guards = {{MakeIdentity<BlackboardKeyId>(81), StateMachineGuardOperator::Present}};
        auto instance = fixture.Instance(source);
        CHECK(instance->Step(1, fixture.binding, fixture.Snapshot()).Value().activeState == Node(1));
        source.transitions[0].guards.clear();
        source.transitions[0].event = MakeIdentity<StateMachineEventId>(1);
        source.transitions[1].event = source.transitions[0].event;
        source.maximumTransitionsPerStep = 4;
        instance = fixture.Instance(source);
        CHECK(instance->Step(1, fixture.binding, fixture.Snapshot()).Value().transitions == 0);
        const std::array events{*source.transitions[0].event};
        CHECK(instance->Step(2, fixture.binding, fixture.Snapshot(), events).Value().transitions == 1);
        CHECK(instance->Step(3, fixture.binding, fixture.Snapshot()).Value().activeState == Node(2));
    }

    TEST_CASE("Hierarchical state actions obey entry update exit and common-ancestor ordering", "[unit][ai][state-machine]") {
        using enum StateMachineActionPhase;
        MachineFixture fixture;
        auto instance = fixture.Instance(Hierarchy());
        ActionLog log;
        const auto result = instance->Step(1, fixture.binding, fixture.Snapshot(), {}, log.Adapter());
        REQUIRE(result.HasValue());
        REQUIRE(log.count == 6);
        const std::array expectedStates{Node(1), Node(2), Node(1), Node(2), Node(2), Node(3)};
        const std::array expectedPhases{Entry, Entry, Update, Update, Exit, Entry};
        for (std::size_t i = 0; i < log.count; ++i) {
            CHECK(log.facts[i].state == expectedStates[i]);
            CHECK(log.facts[i].phase == expectedPhases[i]);
        }
        CHECK(result.Value().activeState == Node(3));
    }

    TEST_CASE("Hierarchical state leaf transitions outrank ancestor transitions", "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto source = Hierarchy();
        source.transitions.push_back({.id = Node(11), .source = Node(1), .target = Node(1), .priority = 99});
        auto instance = fixture.Instance(source);
        ActionLog log;
        CHECK(instance->Step(1, fixture.binding, fixture.Snapshot(), {}, log.Adapter()).Value().lastTransition == Node(10));
        const auto second = instance->Step(2, fixture.binding, fixture.Snapshot(), {}, log.Adapter());
        REQUIRE(second.HasValue());
        CHECK(second.Value().activeState == Node(2));
        REQUIRE(log.count == 12);
        CHECK(log.facts[8].state == Node(3));
        CHECK(log.facts[9].state == Node(1));
        CHECK(log.facts[9].phase == StateMachineActionPhase::Exit);
        CHECK(log.facts[10].state == Node(1));
        CHECK(log.facts[10].phase == StateMachineActionPhase::Entry);
    }

    TEST_CASE("Failed state entry exit and update preserve an immutable coherent terminal", "[unit][ai][state-machine]") {
        using enum StateMachineActionPhase;
        MachineFixture fixture;
        for (const auto phase : {Entry, Exit, Update}) {
            auto instance = fixture.Instance(Hierarchy());
            ActionLog log;
            log.failState = phase == Entry ? Node(3) : Node(2);
            log.failPhase = phase;
            ExpectError(instance->Step(1, fixture.binding, fixture.Snapshot(), {}, log.Adapter()), AIErrors::RuntimeUnavailable);
            CHECK_FALSE(instance->ActiveState().IsValid());
            REQUIRE(instance->TerminalResult() != nullptr);
            CHECK(instance->TerminalResult()->state == AiTaskState::Failed);
            CHECK(instance->TerminalResult()->failure->cause.code.Value() == AIErrors::RuntimeUnavailable.code.Value());
            const auto calls = log.count;
            REQUIRE(instance->Cancel(AiTaskCancellationReason::Requested).HasValue());
            CHECK(instance->TerminalResult()->state == AiTaskState::Failed);
            CHECK(instance->Step(2, fixture.binding, fixture.Snapshot(), {}, log.Adapter()).Value().disposition ==
                  StateMachineStepDisposition::Terminal);
            CHECK(log.count == calls);
        }
    }

    TEST_CASE("State-machine chaining bounds cycles self loops and work explicitly", "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto source = Flat();
        source.transitions[1].target = Node(1);
        source.maximumTransitionsPerStep = 8;
        auto instance = fixture.Instance(source);
        const auto loop = instance->Step(1, fixture.binding, fixture.Snapshot());
        REQUIRE(loop.HasValue());
        CHECK(loop.Value().transitions == 1);
        CHECK(loop.Value().disposition == StateMachineStepDisposition::CycleBounded);
        CHECK(loop.Value().lastTransition == Node(11));
        source.transitions[0].target = Node(1);
        instance = fixture.Instance(source);
        CHECK(instance->Step(1, fixture.binding, fixture.Snapshot()).Value().disposition == StateMachineStepDisposition::CycleBounded);
        source = Flat();
        source.maximumTransitionsPerStep = 2;
        instance = fixture.Instance(source);
        CHECK(instance->Step(1, fixture.binding, fixture.Snapshot()).Value().disposition == StateMachineStepDisposition::BudgetBounded);
        source.maximumTransitionsPerStep = 0;
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source), source), AIErrors::DecisionAssetLimitExceeded);
    }

    TEST_CASE("State-machine retirement fences exact agent schema and instance generations", "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto snapshot = fixture.Snapshot();
        for (const auto change : {0, 1, 2, 3}) {
            auto instance = fixture.Instance(Flat());
            auto current = fixture.binding;
            if (change == 0)
                ++current.agent.slot.generation;
            if (change == 1)
                current.agent.incarnation = AiRuntimeIncarnation::Create(99).Value();
            if (change == 2)
                ++current.schemaGeneration;
            if (change == 3)
                ++current.instanceGeneration;
            const auto result = instance->Step(1, current, snapshot);
            REQUIRE(instance->TerminalResult() != nullptr);
            CHECK_FALSE(instance->ActiveState().IsValid());
            if (change < 2) {
                REQUIRE(result.HasValue());
                CHECK(instance->TerminalResult()->state == AiTaskState::Cancelled);
            } else
                ExpectError(result, AIErrors::BlackboardInstanceStale);
        }
        auto instance = fixture.Instance(Flat());
        REQUIRE(fixture.blackboard->TeardownAtBlackboardSync().HasValue());
        ExpectError(instance->Step(1, fixture.binding, snapshot), AIErrors::BlackboardInstanceStale);
    }

    TEST_CASE("State-machine cancellation and callback invalidation stop publication and later actions", "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto instance = fixture.Instance(Hierarchy());
        ActionLog log;
        log.cancel = &fixture.cancellation;
        REQUIRE(instance->Step(1, fixture.binding, fixture.Snapshot(), {}, log.Adapter()).HasError());
        CHECK(log.count == 1);
        CHECK(instance->TerminalResult()->state == AiTaskState::Cancelled);
        CHECK_FALSE(instance->ActiveState().IsValid());
        instance = fixture.Instance(Hierarchy());
        CHECK(instance->Step(1, fixture.binding, fixture.Snapshot(), {}, log.Adapter()).Value().disposition ==
              StateMachineStepDisposition::Terminal);
        MachineFixture other;
        instance = other.Instance(Hierarchy());
        ActionLog retiredLog;
        retiredLog.retire = other.blackboard.get();
        REQUIRE(instance->Step(1, other.binding, other.Snapshot(), {}, retiredLog.Adapter()).HasError());
        CHECK(retiredLog.count == 1);
        CHECK_FALSE(instance->ActiveState().IsValid());
    }

    TEST_CASE("State-machine callbacks cannot reenter and invalid adapters do not consume a step", "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto instance = fixture.Instance(Hierarchy());
        ExpectError(instance->Step(1, fixture.binding, fixture.Snapshot()), AIErrors::TaskContextInvalid);
        ActionLog log;
        log.runner = instance.get();
        REQUIRE(instance->Step(1, fixture.binding, fixture.Snapshot(), {}, log.Adapter()).HasValue());
        CHECK(log.reentrantRejected);
        CHECK(log.stepRejected);
        const std::array<StateMachineEventId, StateMachineLimits::EventsPerStep + 1> events{};
        ExpectError(instance->Step(2, fixture.binding, fixture.Snapshot(), events, log.Adapter()), StateMachineErrors::StepInvalid);
    }

    TEST_CASE("State-machine compile rejects hierarchy cycles missing initial children and wrong plan families",
              "[unit][ai][state-machine]") {
        MachineFixture fixture;
        auto source = Hierarchy();
        source.states[0].parent = Node(2);
        source.states[1].initialChild = Node(1);
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source), source), StateMachineErrors::HierarchyInvalid);
        source = Hierarchy();
        source.states[0].initialChild = {};
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source), source), StateMachineErrors::TopologyInvalid);
        source = Flat();
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source, true, DecisionPlanKind::BehaviorTree), source),
                    AIErrors::DecisionAssetSchemaInvalid);
        source.transitions[0].target = Node(99);
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source), source), StateMachineErrors::TopologyInvalid);
    }

    TEST_CASE("State-machine admission rejects incomplete node partitions malformed actions and bounded hierarchies",
              "[unit][ai][state-machine]") {
        MachineFixture fixture;
        const auto baseline = Flat();
        const auto decision = fixture.Decision(baseline);
        auto source = baseline;
        source.states[0].entry = TaskId{};
        ExpectError(CookedStateMachinePlan::Compile(decision, source), AIErrors::DecisionAssetSchemaInvalid);
        source = baseline;
        source.states[0].parent = Node(99);
        ExpectError(CookedStateMachinePlan::Compile(decision, source), StateMachineErrors::TopologyInvalid);
        source = baseline;
        source.states.pop_back();
        ExpectError(CookedStateMachinePlan::Compile(decision, source), StateMachineErrors::TopologyInvalid);
        source = baseline;
        source.states[1].id = Node(1);
        ExpectError(CookedStateMachinePlan::Compile(decision, source), AIErrors::DescriptorConflict);
        source = baseline;
        source.transitions[0].event = StateMachineEventId{};
        ExpectError(CookedStateMachinePlan::Compile(decision, source), StateMachineErrors::TopologyInvalid);
        source = baseline;
        source.initial = Node(99);
        ExpectError(CookedStateMachinePlan::Compile(decision, source), StateMachineErrors::TopologyInvalid);
        source = baseline;
        source.states.clear();
        ExpectError(CookedStateMachinePlan::Compile(decision, source), AIErrors::DecisionAssetSchemaInvalid);
        source = {.initial = Node(1)};
        for (std::size_t i = 1; i <= StateMachineLimits::Depth + 1; ++i)
            source.states.push_back({.id = Node(i),
                                     .parent = i == 1 ? DecisionNodeId{} : Node(i - 1),
                                     .initialChild = i == StateMachineLimits::Depth + 1 ? DecisionNodeId{} : Node(i + 1)});
        ExpectError(CookedStateMachinePlan::Compile(fixture.Decision(source), source), StateMachineErrors::HierarchyInvalid);
        source.states.pop_back();
        source.states.back().initialChild = {};
        auto instance = fixture.Instance(source);
        CHECK(instance->Step(1, fixture.binding, fixture.Snapshot()).Value().activeState == Node(StateMachineLimits::Depth));
        auto plan = CookedStateMachinePlan::Compile(decision, baseline);
        REQUIRE(plan.HasValue());
        const AiTaskOperationContext operation{MakeIdentity<TaskId>(90),
                                               {fixture.binding.agent.incarnation, {8, 1}},
                                               fixture.binding.agent,
                                               fixture.cancellation.Token()};
        ExpectError(StateMachineInstance::Create(nullptr, fixture.binding, operation), AIErrors::TaskContextInvalid);
        auto foreign = fixture.binding;
        foreign.schema = MakeIdentity<BlackboardSchemaId>(99);
        ExpectError(StateMachineInstance::Create(plan.Value(), foreign, operation), AIErrors::TaskContextInvalid);
    }
}  // namespace Horo::AI
