#include "DecisionGraphCompilerTestSupport.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/LuaDecisionNode.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "OwnedTestDirectory.h"

#include <cstring>
#include <fstream>

namespace Horo::AI {
    namespace {
        using namespace DecisionGraphTestSupport;

        Gameplay::GameModuleLoadExpectation NativeExpectation() {
            std::ifstream revisionFile(HORO_TEST_AI_MODULE_REVISION_PATH);
            std::uint64_t revision{};
            revisionFile >> revision;
            REQUIRE(revision != 0);
            Gameplay::GameModuleLoadExpectation expected{"game.ai.tests", Gameplay::CurrentGameplayBuildFingerprint(), revision};
            return expected;
        }

        std::unique_ptr<Gameplay::LoadedGameModule> NativeModule(const std::filesystem::path &shadow = {}) {
            Gameplay::GameModuleHost host;
            auto loaded = shadow.empty() ? host.Load(HORO_TEST_AI_MODULE_PATH, NativeExpectation())
                                         : host.LoadShadowCopy(HORO_TEST_AI_MODULE_PATH, shadow, NativeExpectation());
            REQUIRE(loaded.HasValue());
            REQUIRE_FALSE(loaded.Value().diagnostic);
            return std::move(loaded).Value().owner;
        }

        void ObserveNativeLifecycle(const std::filesystem::path &path, std::array<std::uint32_t, 3> &counters) {
            auto image = Platform::LoadDynamicLibrary(path.string());
            REQUIRE(image.HasValue());
            void *symbol = image.Value()->GetSymbol("HoroTestObserveDecisionNodeLifecycle");
            REQUIRE(symbol != nullptr);
            using Observe = void (*)(std::uint32_t *) noexcept;
            Observe observe{};
            static_assert(sizeof(observe) == sizeof(symbol));
            std::memcpy(&observe, &symbol, sizeof(observe));
            observe(counters.data());
        }  // The extra loader reference is released before any shutdown/quiescence assertion.

        TEST_CASE("A compiled authored call closure executes actual generated native nodes and pins their code image",
                  "[ai][decision-compiler-provider]") {
            Fixture fixture;
            auto module = NativeModule();
            auto acquired = module->AcquireDecisionNodes();
            REQUIRE(acquired.HasValue());
            auto generation = std::move(acquired).Value();
            auto environment = fixture.Environment();
            environment.nodes = generation;
            const std::array sources{CallTree(1, 2, DecisionPlanKind::StateMachine), CallingMachine(2, 3, DecisionPlanKind::BehaviorTree),
                                     TaskTree(3, 101)};
            auto compiled = DecisionGraphCompiler::Compile(Graph(1), sources, environment);
            REQUIRE(compiled.HasValue());
            REQUIRE(compiled.Value().plan);
            auto candidate = std::move(compiled).Value();
            auto plan = std::move(candidate.plan);
            CHECK(plan->Storage().unknownNativeProviders == 1);
            auto created = DecisionGraphInstance::Create(plan, generation, fixture.RuntimeBinding());
            REQUIRE(created.HasValue());
            auto instance = std::move(created).Value();
            CHECK(fixture.Step(*instance, 1) == AiTaskState::Running);
            CHECK(fixture.Step(*instance, 2) == AiTaskState::Succeeded);
            CHECK(module->PrepareReload().HasError());
            CHECK_FALSE(plan->IsCurrent());
            CHECK(fixture.Step(*instance, 3) == AiTaskState::Succeeded);  // The previously published terminal cannot be rewritten.
            instance.reset();
            CHECK(module->TryRetire().HasError());
            plan.reset();
            environment.nodes.reset();
            generation.reset();
            CHECK(module->TryRetire().HasValue());
        }

        TEST_CASE("Destroying the actual native owner revokes compiled admission while retained tasks clean up exactly once before unload",
                  "[ai][decision-compiler-provider]") {
            std::array<std::uint32_t, 3> counters{};
            Horo::Tests::OwnedTestDirectory shadows{"decision-native-shadow"};
            Fixture fixture;
            auto module = NativeModule(shadows.Path());
            const auto shadow = module->LoadedArtifactPath();
            ObserveNativeLifecycle(shadow, counters);
            auto acquired = module->AcquireDecisionNodes();
            REQUIRE(acquired.HasValue());
            auto generation = std::move(acquired).Value();
            auto environment = fixture.Environment();
            environment.nodes = generation;
            const std::array sources{TaskTree(1, 101)};
            auto compiled = DecisionGraphCompiler::Compile(Graph(1), sources, environment);
            REQUIRE(compiled.HasValue());
            REQUIRE(compiled.Value().plan);
            auto candidate = std::move(compiled).Value();
            auto plan = std::move(candidate.plan);
            auto created = DecisionGraphInstance::Create(plan, generation, fixture.RuntimeBinding());
            REQUIRE(created.HasValue());
            auto instance = std::move(created).Value();
            REQUIRE(fixture.Step(*instance, 1) == AiTaskState::Running);
            CHECK(counters[0] == 1);
            module.reset();
            CHECK_FALSE(generation->CanUse());
            CHECK_FALSE(generation->CanAdmit());
            CHECK_FALSE(plan->IsCurrent());
            auto rejected = DecisionGraphCompiler::Compile(Graph(1), sources, environment);
            REQUIRE(rejected.HasValue());
            CHECK_FALSE(rejected.Value().plan);
            CHECK(generation
                      ->CreateExecutor(generation, *std::get<std::shared_ptr<const BehaviorTreeExecutionPlan>>(plan->Graphs()[0].execution),
                                       {})
                      .HasError());
            CHECK(fixture.Step(*instance, 2) == AiTaskState::Cancelled);
            CHECK(counters[0] == 1);
            CHECK(counters[1] == 1);
            CHECK(counters[2] == 1);
            instance.reset();
            CHECK(counters[2] == 1);
            CHECK(std::filesystem::exists(shadow));
            plan.reset();
            environment.nodes.reset();
            generation.reset();
            CHECK_FALSE(std::filesystem::exists(shadow));
            CHECK(counters[2] == 1);
        }

        TEST_CASE("Compiled repeated Lua subplans own isolated real quota-bound VMs and advance once per committed tick",
                  "[ai][decision-compiler-provider]") {
            Fixture fixture;
            const std::string source = R"(
return {
 role = "task", requirements = {{key="1", kind=0}},
 start = function(self, context) assert(context.blackboard["1"] == true); self.remaining = 2; return 1 end,
 resume = function(self, context) self.remaining = self.remaining - 1; if self.remaining == 0 then return 2 end; return 1 end,
 abort = function(self, context) self.aborted = true end,
 cleanup = function(self, context) self.cleaned = true end
})";
            auto compiledProgram =
                Gameplay::LuaDecisionNodeProgram::Compile(source, Type(601), MakeIdentity<DecisionProviderId>(51), 1, {__FILE__, 0, 0});
            REQUIRE(compiledProgram.HasValue());
            auto program = std::move(compiledProgram).Value();
            auto acquired = program->Acquire();
            REQUIRE(acquired.HasValue());
            auto generation = std::move(acquired).Value();
            auto environment = fixture.Environment();
            environment.nodes = generation;
            const std::array sources{ParallelCalls(1, 2, DecisionPlanKind::StateMachine),
                                     CallingMachine(2, 3, DecisionPlanKind::BehaviorTree), TaskTree(3, 601)};
            DecisionCompilationLimits limits;
            limits.memoryPolicy = DecisionMemoryPolicy::EnforcedTotal;
            auto compiled = DecisionGraphCompiler::Compile(Graph(1), sources, environment, limits);
            REQUIRE(compiled.HasValue());
            REQUIRE(compiled.Value().plan);
            auto plan = compiled.Value().plan;
            CHECK(plan->Storage().unknownNativeProviders == 0);
            CHECK(plan->Storage().nativeDeclaredProviders == 0);
            CHECK(plan->Storage().enforcedVmBytes == 2 * generation->Contracts()[0].memory.enforcedVmBytes);
            auto created = DecisionGraphInstance::Create(plan, generation, fixture.RuntimeBinding());
            REQUIRE(created.HasValue());
            auto instance = std::move(created).Value();
            CHECK(fixture.Step(*instance, 1) == AiTaskState::Running);
            CHECK(fixture.Step(*instance, 2) == AiTaskState::Running);
            CHECK(fixture.Step(*instance, 3) == AiTaskState::Succeeded);
            program->Retire();
            CHECK_FALSE(plan->IsCurrent());
            CHECK(fixture.Step(*instance, 4) == AiTaskState::Succeeded);
        }
    }  // namespace
}  // namespace Horo::AI
