#include "../runtime/prefab/PrefabValueOverrideTestSupport.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "SceneDocumentTestSupport.h"
#include "editor/document/RuntimeSceneConversion.h"

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Prefab::RuntimeValueTestSupport;

    /** @brief Real source, schema and canonical Scene/history owners; no independently mutable instance model. */
    struct Owner final {
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        SceneDocumentPrefabValueCommands values{document, history};
        Prefab::PrefabInstanceId instance;
        Prefab::PrefabSourceResolverSnapshot sources = Sources();
        Prefab::PrefabValueSchemaSnapshot schemas =
            Prefab::PrefabValueSchemaSnapshot::Create({}, Limits(),
                                                      {{Gameplay::ComponentDescriptor{Type(), 1, "Navigation", "Runtime", {}, {}},
                                                        1,
                                                        true,
                                                        true,
                                                        {},
                                                        {},
                                                        {}}})
                .Value();

        Owner() {
            auto created =
                commands.Execute(CreateScenePrefabInstanceCommand{Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}});
            REQUIRE(created.HasValue());
            instance = *created.Value().prefabInstance;
        }

        EditScenePrefabComponentsCommand Batch() {
            const auto source = sources.Resolve(Asset(), instance, Limits()).Value();
            SceneSource::SceneObjectComponentSet components;
            components.navigationAgent =
                Runtime::NavigationAgentComponent{.profile = Navigation::NavigationAgentProfileId::Create(1).Value(),
                                                  .filter = Navigation::NavigationFilterId::Create(1).Value(),
                                                  .radiusOverride = 4};
            auto replacement =
                SceneSource::EncodePrefabNavigationComponent(Runtime::BuiltinSceneComponentKind::NavigationAgent,
                                                             Prefab::PrefabComponentInstanceId::Create(2).Value(), components);
            REQUIRE(replacement.HasValue());
            return {instance,
                    document.Revision(),
                    source.Revision(),
                    schemas.Revision(),
                    {Prefab::PrefabReplaceComponentEdit{{Prefab::ValueTestSupport::Object(), Type(),
                                                         Prefab::PrefabComponentInstanceId::Create(1).Value()},
                                                        replacement.Value()}},
                    document.ReplacementEpoch()};
        }
    };
}  // namespace

TEST_CASE("Navigation Prefab component batch owns one undoable override gesture", "[editor][navigation][prefab][inspector]") {
    Owner owner;
    auto command = owner.Batch();
    const auto before = owner.document.Revision();
    auto second = std::get<Prefab::PrefabReplaceComponentEdit>(command.edits.front());
    second.target.instance = second.replacement.instance;
    second.replacement.instance = Prefab::PrefabComponentInstanceId::Create(3).Value();
    command.edits.push_back(second);
    auto result = owner.values.Execute(command, owner.sources, &owner.schemas, Limits());
    REQUIRE(result.HasValue());
    CHECK(result.Value().committed);
    CHECK(owner.document.Revision().value == before.value + 1);
    const auto retained = owner.document.PrefabInstances()[0].valueOverrides;
    REQUIRE(retained);
    CHECK(retained->componentRecords.size() == 2);
    auto runtime =
        ConvertSceneDocumentToRuntime(owner.document.Snapshot(), Runtime::SceneDefinitionId{7}, owner.sources, Limits(), &owner.schemas);
    REQUIRE(runtime.HasValue());
    REQUIRE(runtime.Value().Entities().size() == 1);
    CHECK(runtime.Value().Entities()[0].components.navigationAgent->radiusOverride == 4);
    REQUIRE(owner.commands.Undo().HasValue());
    CHECK_FALSE(owner.document.PrefabInstances()[0].valueOverrides);
    REQUIRE(owner.commands.Redo().HasValue());
    CHECK(owner.document.PrefabInstances()[0].valueOverrides == retained);
}

TEST_CASE("Rejected Navigation Prefab batch preserves every override and history fence", "[editor][navigation][prefab][inspector]") {
    Owner owner;
    auto command = owner.Batch();
    const auto before = owner.document.Revision();
    SECTION("last operation invalid") {
        auto bad = std::get<Prefab::PrefabReplaceComponentEdit>(command.edits.front());
        bad.target.instance = Prefab::PrefabComponentInstanceId::Create(999).Value();
        bad.replacement.instance = Prefab::PrefabComponentInstanceId::Create(3).Value();
        command.edits.push_back(bad);
    }
    SECTION("stale document") {
        ++command.expectedRevision.value;
    }
    SECTION("replaced opening") {
        ++command.expectedReplacementEpoch;
    }
    SECTION("stale schema") {
        command.expectedSchema = {};
    }
    SECTION("stale source") {
        ++command.expectedSource.registry.value;
    }
    CHECK(owner.values.Execute(command, owner.sources, &owner.schemas, Limits()).HasError());
    CHECK(owner.document.Revision() == before);
    CHECK_FALSE(owner.document.PrefabInstances()[0].valueOverrides);
}

TEST_CASE("Canonical navigation Prefab writer rejects invalid drafts before producing bytes", "[editor][navigation][prefab][codec]") {
    using namespace Horo;
    SceneSource::SceneObjectComponentSet components;
    components.navigationAgent = Runtime::NavigationAgentComponent{.profile = Navigation::NavigationAgentProfileId::Create(1).Value(),
                                                                   .filter = Navigation::NavigationFilterId::Create(1).Value(),
                                                                   .radiusOverride = -1};
    const auto occurrence = Prefab::PrefabComponentInstanceId::Create(2).Value();
    CHECK(SceneSource::EncodePrefabNavigationComponent(Runtime::BuiltinSceneComponentKind::NavigationAgent, occurrence, components)
              .HasError());
    CHECK(SceneSource::EncodePrefabNavigationComponent(Runtime::BuiltinSceneComponentKind::NavigationSurface, occurrence, components)
              .HasError());
    CHECK(SceneSource::EncodePrefabNavigationComponent(Runtime::BuiltinSceneComponentKind::Camera, occurrence, components).HasError());
}
