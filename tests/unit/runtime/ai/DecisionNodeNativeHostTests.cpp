#include "BehaviorTreeRuntimeTestSupport.h"
#include "Horo/AI/DecisionNodeRegistration.h"
#include "Horo/Gameplay/GameModuleHost.h"

#include <fstream>

namespace Horo::AI {
    namespace {
        Gameplay::GameModuleLoadExpectation Expectation() {
            std::ifstream input(HORO_TEST_AI_MODULE_REVISION_PATH);
            std::uint64_t revision{};
            input >> revision;
            REQUIRE(revision != 0);
            return {"game.ai.tests", Gameplay::CurrentGameplayBuildFingerprint(), revision};
        }

        std::unique_ptr<Gameplay::LoadedGameModule> Load() {
            auto result = Gameplay::GameModuleHost{}.Load(HORO_TEST_AI_MODULE_PATH, Expectation());
            REQUIRE(result.HasValue());
            REQUIRE_FALSE(result.Value().diagnostic);
            return std::move(result).Value().owner;
        }

        TEST_CASE("Generated native decision node callbacks pin their actual image through retirement", "[ai][decision-node][native]") {
            auto module = Load();
            auto acquired = module->AcquireDecisionNodes();
            REQUIRE(acquired.HasValue());
            auto generation = std::move(acquired).Value();
            REQUIRE(generation->Contracts().size() == 1);
            CHECK(generation->Contracts()[0].descriptor.type.Value() == 101);
            CHECK(generation->Contracts()[0].descriptor.source.source.ends_with("AiGameModule.cpp"));
            CHECK(generation->Contracts()[0].descriptor.source.line != 0);
            BehaviorTreeTestSupport::Fixture fixture({BehaviorTreeTestSupport::Node(1, BehaviorTreeOperation::Task)});
            auto executor = generation->CreateExecutor(generation, *fixture.Bindings(), {});
            REQUIRE(executor.HasValue());
            const BehaviorTreeInstanceBinding binding{fixture.agent,
                                                      {},
                                                      100,
                                                      1,
                                                      {fixture.agent, fixture.schema->Identity(), fixture.schema->Version(), 1, 1}};
            auto created = BehaviorTreeInstance::Create(fixture.Plan(), binding, std::move(executor).Value());
            REQUIRE(created.HasValue());
            auto instance = std::move(created).Value();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            CHECK(module->PrepareReload().HasError());
            CHECK_FALSE(generation->CanAdmit());
            CHECK(module->AcquireDecisionNodes().HasError());
            instance->Shutdown();
            instance.reset();
            CHECK(module->TryRetire().HasError());  // Inert inspection lease still owns its executable generation.
            generation.reset();
            CHECK(module->TryRetire().HasValue());
        }

        TEST_CASE("Generated native node factory exceptions reject the whole candidate without unloading live callbacks",
                  "[ai][decision-node][native]") {
            auto module = Load();
            auto acquired = module->AcquireDecisionNodes();
            REQUIRE(acquired.HasValue());
            auto generation = std::move(acquired).Value();
            BehaviorTreeTestSupport::Fixture fixture({BehaviorTreeTestSupport::Node(1, BehaviorTreeOperation::Task)});
            auto properties = generation->Contracts()[0].properties;
            properties[0].value = true;
            const std::array configuration{DecisionNodeConfiguration{AI::DecisionNodeId::Create(1).Value(), properties}};
            CHECK(generation->CreateExecutor(generation, *fixture.Bindings(), {}, configuration).HasError());
            CHECK(generation->CreateExecutor(generation, *fixture.Bindings(), {}).HasValue());
            generation.reset();
            CHECK(module->TryRetire().HasValue());
        }
    }  // namespace
}  // namespace Horo::AI
