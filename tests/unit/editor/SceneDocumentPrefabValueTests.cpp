#include "../runtime/prefab/PrefabValueOverrideTestSupport.h"
#include "Horo/Scene/SceneSource.h"
#include "SceneDocumentTestSupport.h"
#include "TriggerVolumeConversionTestValues.h"

namespace Horo::Editor {
    namespace {
        using namespace Prefab::RuntimeValueTestSupport;

        /** @brief Verify one rejected operation preserves both committed revision and the exact owned authoring intent. */
        void CheckRejectedEdit(const SceneDocument &document, const Result<SceneCommandResult> &result, const DocumentRevision revision,
                               const Prefab::PrefabValueOverrideSet &retained) {
            CHECK(result.HasError());
            CHECK(document.Revision() == revision);
            CHECK(document.PrefabInstances()[0].valueOverrides == retained);
        }

        /** @brief Check editor adapters and canonical persistence against the same committed typed override state. */
        void CheckSceneValueProjection(const SceneDocument &document, const Prefab::PrefabSourceResolverSnapshot &sources,
                                       const Prefab::PrefabValueSchemaSnapshot &schemas, const Prefab::PrefabValueOverrideSet &retained) {
            const auto snapshot = document.Snapshot();
            auto preview = BuildScenePrefabProjection(snapshot, sources, Limits(), &schemas);
            REQUIRE(preview.HasValue());
            REQUIRE(preview.Value().instances[0].expanded);
            CHECK(preview.Value().instances[0].valueProvenance.back().origin == Prefab::PrefabValueOrigin::Instance);
            REQUIRE(ConvertScenePrefabProjectionToRuntime(snapshot, {20}, preview.Value(), sources, Limits(), &schemas).HasValue());
            auto runtime = ConvertSceneDocumentToRuntime(snapshot, {20}, sources, Limits(), &schemas);
            REQUIRE(runtime.HasValue());
            REQUIRE(runtime.Value().Entities().size() == 1);
            const auto &agent = runtime.Value().Entities()[0].components.navigationAgent;
            CHECK_FALSE(agent);  // Canonical runtime conversion omits explicitly disabled Navigation components.
            const auto bytes = SceneSource::EncodeSceneSource({document.Objects(), document.PrefabInstances()});
            auto decoded = SceneSource::DecodeSceneSource(bytes);
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value().prefabInstances.size() == 1);
            CHECK(decoded.Value().prefabInstances[0].valueOverrides == retained);
        }

        TEST_CASE("Prefab value edit and history epochs reject canonical reload with repeated revisions",
                  "[unit][editor][prefab][lifecycle]") {
            SceneDocument document;
            EditorHistory history;
            SceneDocumentCommandExecutor commands{document, history};
            SceneDocumentPrefabValueCommands values{document, history};
            const auto instance = Prefab::PrefabInstanceId::Create(9).Value();
            std::vector<ScenePrefabInstance> placements{{instance, Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}}};
            REQUIRE(document.LoadSaved({}, placements).HasValue());
            const auto sources = Sources();
            const auto schemas = Schemas();
            const auto source = sources.Resolve(Asset(), instance, Limits()).Value();
            EditScenePrefabValueCommand old{instance,
                                            document.Revision(),
                                            source.Revision(),
                                            schemas.Revision(),
                                            {Prefab::PrefabValueEditKind::Assign, Property(), false},
                                            document.ReplacementEpoch()};
            REQUIRE(values.Execute(old, sources, &schemas, Limits()).HasValue());
            REQUIRE(document.LoadSaved({}, placements).HasValue());
            CHECK(document.Revision() == old.expectedRevision);
            CHECK(document.ReplacementEpoch() != old.expectedReplacementEpoch);
            CHECK(values.Execute(old, sources, &schemas, Limits()).HasError());
            CHECK(commands.Undo().HasError());
            CHECK_FALSE(document.PrefabInstances()[0].valueOverrides);
            const auto epoch = document.ReplacementEpoch();
            auto invalid = placements;
            invalid[0].parent = SceneObjectId{99};
            CHECK(document.LoadSaved({}, invalid).HasError());
            CHECK(document.ReplacementEpoch() == epoch);
            old.expectedReplacementEpoch = epoch;
            REQUIRE(values.Execute(old, sources, &schemas, Limits()).HasValue());
            REQUIRE(document.PrefabInstances()[0].valueOverrides);
        }

        TEST_CASE("Prefab value document commands commit revert and replay exact semantic intent", "[unit][editor][prefab][history]") {
            SceneDocument document;
            EditorHistory history;
            SceneDocumentCommandExecutor commands{document, history};
            SceneDocumentPrefabValueCommands values{document, history};
            const auto created =
                commands.Execute(CreateScenePrefabInstanceCommand{Prefab::PrefabAssetReference::Create(Asset()).Value(), {}, {}});
            REQUIRE(created.HasValue());
            const auto instance = *created.Value().prefabInstance;
            const auto sources = Sources();
            const auto schemas = Schemas();
            const auto source = sources.Resolve(Asset(), instance, Limits()).Value();
            EditScenePrefabValueCommand command{instance,
                                                document.Revision(),
                                                source.Revision(),
                                                schemas.Revision(),
                                                {Prefab::PrefabValueEditKind::Assign, Property(), false},
                                                document.ReplacementEpoch()};
            const auto before = document.Revision();
            auto assigned = values.Execute(command, sources, &schemas, Limits());
            REQUIRE(assigned.HasValue());
            REQUIRE(assigned.Value().committed);
            CHECK(document.Revision().value == before.value + 1);
            REQUIRE(document.PrefabInstances()[0].valueOverrides);
            const auto retained = *document.PrefabInstances()[0].valueOverrides;
            CheckSceneValueProjection(document, sources, schemas, retained);
            REQUIRE(commands.Undo().HasValue());
            CHECK_FALSE(document.PrefabInstances()[0].valueOverrides);
            REQUIRE(commands.Redo().HasValue());
            CHECK(document.PrefabInstances()[0].valueOverrides == retained);
            CHECK(values.Execute(command, sources, &schemas, Limits()).HasError());
            CHECK(document.PrefabInstances()[0].valueOverrides == retained);
            command.expectedRevision = document.Revision();
            command.edit.kind = Prefab::PrefabValueEditKind::Revert;
            REQUIRE(values.Execute(command, sources, &schemas, Limits()).HasValue());
            CHECK_FALSE(document.PrefabInstances()[0].valueOverrides);
            REQUIRE(commands.Undo().HasValue());
            CHECK(document.PrefabInstances()[0].valueOverrides == retained);
            const auto unchanged = document.Revision();
            command.expectedRevision = unchanged;
            command.edit.kind = Prefab::PrefabValueEditKind::Assign;
            command.edit.value = 11.0;
            CHECK(values.Execute(command, Sources(2.0, 2), &schemas, Limits()).HasError());
            CheckRejectedEdit(document, values.Execute(command, sources, &schemas, Limits()), unchanged, retained);
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            command.edit.value = 3.0;
            CheckRejectedEdit(document, values.Execute(command, sources, &schemas, Limits(), cancelled.Token()), unchanged, retained);
        }
    }  // namespace
}  // namespace Horo::Editor

TEST_CASE("Editor prefab projection keeps its schema signature and runtime trigger binding", "[unit][editor][prefab][physics]") {
    using namespace Horo;
    using ProjectionFunction =
        Result<Editor::ScenePrefabProjection> (*)(const Editor::SceneDocumentSnapshot &, const Prefab::PrefabSourceResolverSnapshot &,
                                                  const Prefab::PrefabLimitProfile &, const Prefab::PrefabValueSchemaSnapshot *);
    const ProjectionFunction build = &Editor::BuildScenePrefabProjection;
    Assets::AssetRegistry registry;
    const auto limits = Editor::SceneDocumentTestSupport::ScenePrefabLimits();
    const auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {}, limits).Value();
    const Editor::SceneDocumentSnapshot document{.revision = Editor::DocumentRevision{1},
                                                 .state = Editor::DocumentStateId{1},
                                                 .objects = {
                                                     Editor::SceneObjectSnapshot{.id = Editor::SceneObjectId{1},
                                                                                 .name = "Bound trigger",
                                                                                 .components = {.triggerVolume =
                                                                                                    Runtime::TriggerVolumeComponent{}}}}};
    const Prefab::PrefabValueSchemaSnapshot *schemas = nullptr;
    const auto projection = build(document, resolver, limits, schemas);
    REQUIRE(projection.HasValue());
    const auto binding = Test::TriggerBinding();
    const auto retained =
        Editor::ConvertScenePrefabProjectionToRuntime(document, {1}, projection.Value(), resolver, limits, schemas, binding);
    const auto direct = Editor::ConvertSceneDocumentToRuntime(document, {1}, resolver, limits, schemas, binding);
    REQUIRE(retained.HasValue());
    REQUIRE(direct.HasValue());
    REQUIRE(retained.Value().TriggerVolumeEvidence().size() == 1);
    REQUIRE(direct.Value().TriggerVolumeEvidence().front().binding == binding.binding);
    REQUIRE(retained.Value().TriggerVolumeEvidence().front().binding == binding.binding);
    REQUIRE(Editor::ConvertSceneDocumentToRuntime(document, {1}, resolver, limits, schemas).HasError());
}
