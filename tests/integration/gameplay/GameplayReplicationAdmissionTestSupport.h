#pragma once
/** @file GameplayReplicationAdmissionTestSupport.h @brief Actual file-backed Lua generation for application admission regressions. */
#include "../../support/OwnedTestDirectory.h"
#include "GameplayWorldComposition.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <fstream>

namespace Horo::Application::Internal::TestSupport {
    using namespace Horo::Gameplay;

    /** @brief Owns real script files and complete canonical requirements copied from its compiled generation. */
    struct ReplicatedScriptFixture final {
        Tests::OwnedTestDirectory directory{"replication admission"};
        GameplayWorldSelection selection{.moduleId = "game.tests",
                                         .scriptSource = directory.Path() / "behavior.horo_script",
                                         .scriptSidecar = directory.Path() / "behavior.horo_script.meta"};

        explicit ReplicatedScriptFixture(const bool failOnCreate = false) {
            {
                std::ofstream source{selection.scriptSource};
                source << R"(return horo.behavior { display_name="Replicated", fields={},
                    replication={major=1,minor=0,minimum_minor=0,maximum_minor=0,
                    capture_phase="gameplay",apply_phase="pre_physics",fields={
                    {id=7,value_type=1,codec=2,kind="number",introduced_major=1,introduced_minor=0,
                    condition="owner_only",requirement="required",maximum_bytes=8,maximum_elements=1}},tombstones={}},
                    on_create=function(ctx) )";
                if (failOnCreate)
                    source << R"(error("callback before admission"))";
                source << " end }";
                REQUIRE(source.good());
            }
            {
                std::ofstream sidecar{selection.scriptSidecar};
                sidecar << R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.replicated",
                              "replicationSchemaId":42,"replicationModuleId":"game.tests"})";
                REQUIRE(sidecar.good());
            }
            auto program = LuaBehaviorProgram::LoadFiles(selection.scriptSource, selection.scriptSidecar);
            if (program.HasError()) {
                UNSCOPED_INFO(program.ErrorValue().domain.Value());
                UNSCOPED_INFO(program.ErrorValue().code.Value());
                UNSCOPED_INFO(program.ErrorValue().message);
            }
            REQUIRE(program.HasValue());
            selection.replicationAdmission.emplace();
            selection.replicationAdmission->inventory.completeness = Network::NetworkReplicationInventoryCompleteness::Complete;
            selection.replicationAdmission->inventory.declarations.push_back({program.Value()->ReplicationDeclaration()->schema});
            selection.replicationAdmission->use = Network::ReplicationDeclarationUse::ServerActivation;
        }

        /** @brief Write an incompatible real replacement while the active program keeps its immutable declarations. */
        void ChangeCodec() const {
            std::ifstream input{selection.scriptSource};
            std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            input.close();
            const auto codec = source.find("codec=2");
            REQUIRE(codec != std::string::npos);
            source.replace(codec, 7, "codec=3");
            std::ofstream output{selection.scriptSource};
            output << source;
            REQUIRE(output.good());
        }

        std::unique_ptr<Runtime::RuntimeScene> Scene() const {
            Runtime::SceneDefinitionBuilder builder{{1}, {1}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = {1};
            entity.components.behaviors.emplace_back(BehaviorInstanceId{1}, BehaviorTypeId::Parse("game.tests.replicated").Value(), 1, true,
                                                     std::vector<BehaviorField>{});
            builder.Add(std::move(entity));
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            return Runtime::RuntimeScene::Create(definition.Value(), {71}).Value();
        }
    };

}  // namespace Horo::Application::Internal::TestSupport
