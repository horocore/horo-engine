#include "DecisionGraphCompilerTestSupport.h"

#include <algorithm>

namespace Horo::AI {
    namespace {
        using namespace DecisionGraphTestSupport;

        TEST_CASE("Decision graph compiler admits canonical typed controls without synthetic provider descriptors",
                  "[ai][decision-compiler]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            auto plan = fixture.Compile(sources, 1);
            REQUIRE(plan->Graphs().size() == 1);
            CHECK(plan->Graphs()[0].bindings->Nodes()[0].intrinsic == DecisionControlKind::Root);
            CHECK_FALSE(plan->Graphs()[0].bindings->Nodes()[0].type.IsValid());
            CHECK(plan->Graphs()[0].bindings->Nodes()[1].type == Type(501));
            CHECK(plan->Storage().maximumExecutionDepth == 2);
            CHECK(plan->Storage().coreInstanceBytes > 0);
            CHECK(plan->Storage().nativeDeclaredProviders == 1);
            CHECK(fixture.probe.factories == 0);  // Compilation cannot execute or probe native factories.
        }

        TEST_CASE("Decision closure keeps complete dependencies and allocates repeated parallel calls independently",
                  "[ai][decision-compiler]") {
            Fixture fixture;
            const std::array sources{ParallelCalls(1, 2, DecisionPlanKind::StateMachine),
                                     CallingMachine(2, 3, DecisionPlanKind::BehaviorTree), TaskTree(3)};
            auto plan = fixture.Compile(sources, 1);
            REQUIRE(plan->Occurrences().size() == 5);
            CHECK(plan->Storage().maximumCallDepth == 3);
            CHECK(plan->Storage().maximumExecutionDepth >= 6);
            CHECK(plan->Storage().nativeDeclaredProviders == 2);
            REQUIRE(plan->Graphs()[0].bindings->Dependencies().size() == 2);
            CHECK(plan->Occurrences()[1].plan == plan->Occurrences()[3].plan);
            CHECK(plan->Occurrences()[2].plan == plan->Occurrences()[4].plan);
            CHECK(plan->Occurrences()[2].firstTaskSlot != plan->Occurrences()[4].firstTaskSlot);
        }

        TEST_CASE("Invalid decision closure never replaces its last executable plan or invokes a factory", "[ai][decision-compiler]") {
            Fixture fixture;
            const std::array good{TaskTree(1)};
            auto compiled = DecisionGraphCompiler::Compile(Graph(1), good, fixture.Environment());
            REQUIRE(compiled.HasValue());
            DecisionPlanStore store;
            auto activated = store.TryActivate(std::move(compiled).Value());
            REQUIRE(activated.HasValue());
            REQUIRE(activated.Value());
            const auto previous = store.Current();
            const std::array bad{CallTree(1, 2, DecisionPlanKind::StateMachine)};
            auto rejected = DecisionGraphCompiler::Compile(Graph(1), bad, fixture.Environment());
            REQUIRE(rejected.HasValue());
            CHECK_FALSE(rejected.Value().plan);
            CHECK(rejected.Value().validation.HasErrors());
            REQUIRE(store.TryActivate(std::move(rejected).Value()).HasValue());
            CHECK(store.Current() == previous);
            CHECK(std::get<std::shared_ptr<const BehaviorTreeAsset>>(bad[0].body)->Nodes().size() == 2);
            CHECK(fixture.probe.factories == 0);
        }

        TEST_CASE("Decision compiler rejects cross-family cycles unreachable source and ambiguous schema before execution",
                  "[ai][decision-compiler]") {
            Fixture fixture;
            SECTION("real cross-family cycle") {
                const std::array sources{CallTree(1, 2, DecisionPlanKind::StateMachine),
                                         CallingMachine(2, 1, DecisionPlanKind::BehaviorTree)};
                auto result = DecisionGraphCompiler::Compile(Graph(1), sources, fixture.Environment());
                REQUIRE(result.HasValue());
                CHECK_FALSE(result.Value().plan);
                CHECK(result.Value().validation.HasErrors());
            }
            SECTION("unreachable supplied graph") {
                const std::array sources{TaskTree(1), TerminalMachine(2)};
                auto result = DecisionGraphCompiler::Compile(Graph(1), sources, fixture.Environment());
                REQUIRE(result.HasValue());
                CHECK_FALSE(result.Value().plan);
            }
            SECTION("unreachable HSM state") {
                auto source = TerminalMachine(1);
                std::get<StateMachineAssetDescriptor>(source.body)
                    .states.push_back({.id = Node(2), .terminal = DecisionStateTerminal::Succeeded});
                auto result = DecisionGraphCompiler::Compile(Graph(1), std::span{&source, 1}, fixture.Environment());
                REQUIRE(result.HasValue());
                CHECK_FALSE(result.Value().plan);
            }
            SECTION("ambiguous admitted schema") {
                auto environment = fixture.Environment();
                environment.schemas.push_back(fixture.schema);
                const std::array sources{TaskTree(1)};
                auto result = DecisionGraphCompiler::Compile(Graph(1), sources, environment);
                REQUIRE(result.HasValue());
                CHECK_FALSE(result.Value().plan);
            }
            CHECK(fixture.probe.factories == 0);
        }

        TEST_CASE("Decision compiler admits executable availability versions and capabilities from the actual frozen producer",
                  "[ai][decision-compiler]") {
            Fixture fixture;
            auto registration = Registration(fixture.probe);
            SECTION("inspectable descriptor has no factory") {
                registration.factory = {};
            }
            SECTION("provider contract version incompatible") {
                registration.contract.descriptor.origin.version = 2;
            }
            SECTION("missing target capability") {
                registration.contract.capabilities = AiCapabilitySet::Of(AiCapability::Navigation);
            }
            auto captured = DecisionNodeGeneration::Capture({&registration, 1}, fixture.admission);
            REQUIRE(captured.HasValue());
            auto environment = fixture.Environment();
            environment.nodes = std::move(captured).Value();
            const std::array sources{TaskTree(1)};
            auto rejected = DecisionGraphCompiler::Compile(Graph(1), sources, environment);
            REQUIRE(rejected.HasValue());
            CHECK_FALSE(rejected.Value().plan);
            CHECK(rejected.Value().validation.HasErrors());
            CHECK(fixture.probe.factories == 0);
        }

        TEST_CASE("Decision compiler rejects actual future graph schemas and unsupported Utility calls", "[ai][decision-compiler]") {
            Fixture fixture;
            SECTION("graph schema skew") {
                auto source = TaskTree(1);
                ++source.schemaVersion;
                auto candidate = DecisionGraphCompiler::Compile(Graph(1), std::span{&source, 1}, fixture.Environment());
                REQUIRE(candidate.HasValue());
                CHECK_FALSE(candidate.Value().plan);
                CHECK(candidate.Value().validation.HasErrors());
            }
            SECTION("Utility is not silently lowered as BT or HSM") {
                const std::array sources{CallingMachine(1, 2, DecisionPlanKind::Utility), TerminalMachine(2)};
                auto candidate = DecisionGraphCompiler::Compile(Graph(1), sources, fixture.Environment());
                REQUIRE(candidate.HasValue());
                CHECK_FALSE(candidate.Value().plan);
                CHECK(candidate.Value().validation.HasErrors());
            }
            CHECK(fixture.probe.factories == 0);
        }

        TEST_CASE("Decision memory policy distinguishes native declarations from enforced quotas", "[ai][decision-compiler]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            DecisionCompilationLimits limits;
            limits.memoryPolicy = DecisionMemoryPolicy::EnforcedTotal;
            auto strict = DecisionGraphCompiler::Compile(Graph(1), sources, fixture.Environment(), limits);
            REQUIRE(strict.HasValue());
            CHECK_FALSE(strict.Value().plan);
            limits.memoryPolicy = DecisionMemoryPolicy::IncludeTrustedDeclarations;
            auto declared = DecisionGraphCompiler::Compile(Graph(1), sources, fixture.Environment(), limits);
            REQUIRE(declared.HasValue());
            REQUIRE(declared.Value().plan);
            CHECK(declared.Value().plan->Storage().nativeDeclaredBytes == sizeof(Provider));
            auto claim = Registration(fixture.probe);
            claim.contract.descriptor.origin.kind = DecisionDescriptorSourceKind::Script;
            claim.contract.memory = {DecisionProviderMemoryKind::EnforcedVm, 1, 0, 1024};
            CHECK(DecisionNodeGeneration::Capture({&claim, 1}, fixture.admission).HasError());
            limits.maximumCoreBytes = 1;
            auto bounded = DecisionGraphCompiler::Compile(Graph(1), sources, fixture.Environment(), limits);
            REQUIRE(bounded.HasValue());
            CHECK_FALSE(bounded.Value().plan);
        }

        TEST_CASE("Decision layout checks aggregate call occurrences and combined depth before allocating providers",
                  "[ai][decision-compiler]") {
            Fixture fixture;
            std::vector<DecisionGraphSource> sources;
            for (std::uint64_t graph = 1; graph < 10; ++graph)
                sources.push_back(ParallelCalls(graph, graph + 1, DecisionPlanKind::BehaviorTree));
            sources.push_back(TaskTree(10));
            auto tooMany = DecisionGraphCompiler::Compile(Graph(1), sources, fixture.Environment());
            REQUIRE(tooMany.HasValue());
            CHECK_FALSE(tooMany.Value().plan);
            const std::array small{CallTree(1, 2, DecisionPlanKind::BehaviorTree), TaskTree(2)};
            DecisionCompilationLimits limits;
            limits.maximumExecutionDepth = 3;
            auto tooDeep = DecisionGraphCompiler::Compile(Graph(1), small, fixture.Environment(), limits);
            REQUIRE(tooDeep.HasValue());
            CHECK_FALSE(tooDeep.Value().plan);
            CHECK(fixture.probe.factories == 0);
        }
    }  // namespace
}  // namespace Horo::AI
