#include "BehaviorTreeRuntimeTestSupport.h"
#include "Horo/Gameplay/LuaDecisionNode.h"

namespace Horo::AI {
    namespace {
        using BehaviorTreeTestSupport::Fixture;
        using BehaviorTreeTestSupport::Node;
        using TestSupport::MakeIdentity;

        constexpr std::string_view TaskSource = R"(
return {
 role = "task",
 requirements = {{key="10", kind=0}},
 properties = {{id="4", kind=1, value=2}},
 start = function(self, context)
   assert(context.blackboard["10"] == true)
   self.remaining = context.properties["4"]
   return 1
 end,
 resume = function(self, context)
   self.remaining = self.remaining - 1
   if self.remaining == 0 then return 2 end
   return 1
 end,
 abort = function(self, context) self.aborted = true end,
 cleanup = function(self, context) self.cleaned = true end
})";

        std::unique_ptr<Gameplay::LuaDecisionNodeProgram> Program(std::string source = std::string(TaskSource)) {
            auto compiled = Gameplay::LuaDecisionNodeProgram::Compile(std::move(source), MakeIdentity<DecisionNodeTypeId>(101),
                                                                      MakeIdentity<DecisionProviderId>(501), 1, {__FILE__, 0, 0});
            REQUIRE(compiled.HasValue());
            return std::move(compiled).Value();
        }

        std::shared_ptr<const DecisionAssetPlan> Bindings(const Fixture &fixture, const DecisionNodeGeneration &generation) {
            DecisionAssetDescriptor source{.asset = MakeIdentity<DecisionGraphAssetId>(100),
                                           .blackboardSchema = fixture.schema->Identity(),
                                           .source = {__FILE__, __LINE__, 1}};
            for (const auto &node : fixture.nodes)
                source.nodes.push_back(
                    {.id = node.id, .type = generation.Contracts()[0].descriptor.type, .source = {__FILE__, __LINE__, 1}});
            const std::array descriptor{generation.Contracts()[0].descriptor};
            auto result = DecisionAssetCompiler::Compile(source, {}, descriptor, std::array{fixture.schema});
            REQUIRE(result.HasValue());
            for (const auto &diagnostic : result.Value().validation.Diagnostics())
                UNSCOPED_INFO(diagnostic.code.Value() << " " << diagnostic.path << " " << diagnostic.source.source);
            REQUIRE(result.Value().IsValid());
            return result.Value().plan;
        }

        std::unique_ptr<BehaviorTreeInstance> Instance(Fixture &fixture, const std::shared_ptr<const DecisionNodeGeneration> &generation) {
            auto bindings = Bindings(fixture, *generation);
            auto plan = BehaviorTreeExecutionPlan::Compile(bindings, MakeIdentity<DecisionNodeId>(1), fixture.nodes);
            REQUIRE(plan.HasValue());
            auto executor = generation->CreateExecutor(generation, *plan.Value(), {});
            REQUIRE(executor.HasValue());
            const BehaviorTreeInstanceBinding binding{fixture.agent,
                                                      {},
                                                      100,
                                                      1,
                                                      {fixture.agent, fixture.schema->Identity(), fixture.schema->Version(), 1, 1}};
            auto result = BehaviorTreeInstance::Create(plan.Value(), binding, std::move(executor).Value());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        TEST_CASE("Lua decision declarations generate real isolated VM executors on the existing task lifecycle",
                  "[ai][decision-node][lua]") {
            auto program = Program();
            auto acquired = program->Acquire();
            REQUIRE(acquired.HasValue());
            auto generation = std::move(acquired).Value();
            CHECK(generation->Contracts()[0].descriptor.origin.kind == DecisionDescriptorSourceKind::Script);
            CHECK(generation->Contracts()[0].descriptor.source.source == __FILE__);
            CHECK(generation->Contracts()[0].descriptor.source.line == 0);
            CHECK(generation->Contracts()[0].descriptor.requirements[0].source.source == __FILE__);
            Fixture first({Node(1, BehaviorTreeOperation::Task)});
            Fixture second({Node(1, BehaviorTreeOperation::Task)});
            auto firstInstance = Instance(first, generation);
            auto secondInstance = Instance(second, generation);
            CHECK(first.Evaluate(*firstInstance, 1) == AiTaskState::Running);
            CHECK(first.Evaluate(*firstInstance, 2) == AiTaskState::Running);
            CHECK(first.Evaluate(*firstInstance, 3) == AiTaskState::Succeeded);
            CHECK(second.Evaluate(*secondInstance, 1) == AiTaskState::Running);
            CHECK(second.Evaluate(*secondInstance, 2) == AiTaskState::Running);
            CHECK(second.Evaluate(*secondInstance, 3) == AiTaskState::Succeeded);
        }

        TEST_CASE("Lua decision retirement closes callbacks while exact VM generations safely abort and clean up",
                  "[ai][decision-node][lua]") {
            auto program = Program();
            auto acquired = program->Acquire();
            REQUIRE(acquired.HasValue());
            auto generation = std::move(acquired).Value();
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            auto instance = Instance(fixture, generation);
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            program->Retire();
            CHECK(program->Acquire().HasError());
            CHECK_FALSE(generation->CanAdmit());
            CHECK(generation->CreateExecutor(generation, *Bindings(fixture, *generation), {}).HasError());
            program.reset();
            instance->Shutdown();
            instance.reset();
            generation.reset();
        }

        TEST_CASE("Lua decision metadata rejects unsupported fields types holes and enum wraparound", "[ai][decision-node][lua]") {
            const std::array sources{"return {role='unknown'}",
                                     "return {role='task'}",
                                     "return {role='condition',check=7}",
                                     "return {role='condition',check=function() return true end,typo=true}",
                                     "return {role='condition',check=function() return true end,capabilities=-1}",
                                     "return {role='condition',check=function() return true end,ports={{id='1',kind=256}}}",
                                     "return {role='condition',check=function() return true end,ports={oops={id='1'}}}",
                                     "return {role='condition',check=function() return true end,properties={{id='0',kind=0,value=true}}}"};
            for (const auto *source : sources) {
                INFO(source);
                CHECK(Gameplay::LuaDecisionNodeProgram::Compile(source, MakeIdentity<DecisionNodeTypeId>(101),
                                                                MakeIdentity<DecisionProviderId>(501), 1, {__FILE__, 0, 0})
                          .HasError());
            }
        }

        TEST_CASE("Lua decision compilation and callback loops are bounded by actual VM instruction policy", "[ai][decision-node][lua]") {
            CHECK(Gameplay::LuaDecisionNodeProgram::Compile("while true do end", MakeIdentity<DecisionNodeTypeId>(101),
                                                            MakeIdentity<DecisionProviderId>(501), 1, {__FILE__, 0, 0},
                                                            {128U * 1024U, 1000})
                      .HasError());
            auto program = Program("return {role='condition',check=function() while true do end end}");
            auto acquired = program->Acquire();
            REQUIRE(acquired.HasValue());
            Fixture fixture({Node(1, BehaviorTreeOperation::BlackboardCheck, {2}), Node(2, BehaviorTreeOperation::Task)});
            auto bindings = Bindings(fixture, *acquired.Value());
            auto executor = acquired.Value()->CreateExecutor(acquired.Value(), *bindings, {});
            REQUIRE(executor.HasValue());
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const BehaviorTreeEvaluationContext context{MakeIdentity<DecisionNodeId>(1), {}, snapshot.Value(), 1};
            CHECK(executor.Value()->Check(context).HasError());
        }

        TEST_CASE("Lua condition callbacks observe frozen typed blackboard values and reject wrong return kinds",
                  "[ai][decision-node][lua]") {
            auto program =
                Program("return {role='condition',requirements={{key='10',kind=0}},check=function(self,c) return c.blackboard['10'] end}");
            auto acquired = program->Acquire();
            REQUIRE(acquired.HasValue());
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            auto bindings = Bindings(fixture, *acquired.Value());
            auto executor = acquired.Value()->CreateExecutor(acquired.Value(), *bindings, {});
            REQUIRE(executor.HasValue());
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const BehaviorTreeEvaluationContext context{MakeIdentity<DecisionNodeId>(1),
                                                        bindings->BindingsForNode(MakeIdentity<DecisionNodeId>(1)), snapshot.Value(), 1};
            auto checked = executor.Value()->Check(context);
            REQUIRE(checked.HasValue());
            CHECK(checked.Value());
            fixture.SetCondition(false);
            CHECK(executor.Value()->Check(context).Value());  // Existing immutable snapshot remains true.
        }

        TEST_CASE("Lua task status rejects boolean strings overflow and callback errors", "[ai][decision-node][lua]") {
            const std::array bodies{"return true", "return '2'", "return 4294967297", "error('failure')"};
            for (const auto *body : bodies) {
                INFO(body);
                const std::string source = std::string("return {role='task',start=function() ") + body +
                                           " end,resume=function() return 2 end,abort=function() end,cleanup=function() end}";
                auto program = Program(source);
                auto acquired = program->Acquire();
                REQUIRE(acquired.HasValue());
                Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
                auto instance = Instance(fixture, acquired.Value());
                CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Failed);
            }
        }
    }  // namespace
}  // namespace Horo::AI
