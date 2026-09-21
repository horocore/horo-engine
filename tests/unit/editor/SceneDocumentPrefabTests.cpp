#include "SceneDocumentTestSupport.h"

namespace {
    using namespace Horo::Editor::SceneDocumentTestSupport;

    TEST_CASE("Scene prefab placements keep source identity separate from containing-scene root placement", "[unit][editor][prefab]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto parent = commands.Execute(CreateSceneObjectCommand{.name = "Placement Parent"});
        REQUIRE(parent.HasValue());
        const Math::Transform rootTransform{
            .translation = {3.0F, 4.0F, 5.0F},
            .rotation = Math::Quaternion::FromEulerRadians({0.1F, 0.2F, 0.3F}),
            .scale = {2.0F, 2.0F, 2.0F},
        };
        const Prefab::PrefabAssetReference source = PrefabAsset();

        const auto created = commands.Execute(CreateScenePrefabInstanceCommand{source, parent.Value().object, rootTransform});
        REQUIRE(created.HasValue());
        REQUIRE(created.Value().prefabInstance.has_value());
        REQUIRE(document.PrefabInstances().size() == 1);
        const ScenePrefabInstance &instance = document.PrefabInstances().front();
        REQUIRE(instance.instanceId == *created.Value().prefabInstance);
        REQUIRE(instance.sourcePrefab == source);
        REQUIRE(instance.parent == parent.Value().object);
        REQUIRE(instance.rootTransform == rootTransform);
        REQUIRE(document.Objects().size() == 1);
    }

    TEST_CASE("Scene prefab duplication reparent delete and history preserve reference boundaries", "[unit][editor][prefab][history]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto firstParent = commands.Execute(CreateSceneObjectCommand{.name = "First Parent"});
        const auto secondParent = commands.Execute(CreateSceneObjectCommand{.name = "Second Parent"});
        REQUIRE(firstParent.HasValue());
        REQUIRE(secondParent.HasValue());
        const auto created = commands.Execute(CreateScenePrefabInstanceCommand{PrefabAsset(7), firstParent.Value().object,
                                                                               Math::Transform{.translation = {1.0F, 2.0F, 3.0F}}});
        REQUIRE(created.HasValue());
        const Prefab::PrefabInstanceId originalId = *created.Value().prefabInstance;

        const auto duplicated = commands.Execute(DuplicateScenePrefabInstanceCommand{originalId});
        REQUIRE(duplicated.HasValue());
        const Prefab::PrefabInstanceId duplicateId = *duplicated.Value().prefabInstance;
        REQUIRE(duplicateId != originalId);
        REQUIRE(document.PrefabInstances().size() == 2);
        REQUIRE(document.PrefabInstances()[1].sourcePrefab == document.PrefabInstances()[0].sourcePrefab);
        REQUIRE(document.PrefabInstances()[1].rootTransform == document.PrefabInstances()[0].rootTransform);
        REQUIRE(document.PrefabInstances()[1].parent == document.PrefabInstances()[0].parent);

        REQUIRE(commands.Execute(ReparentScenePrefabInstanceCommand{duplicateId, secondParent.Value().object}).HasValue());
        REQUIRE(document.PrefabInstances()[1].instanceId == duplicateId);
        REQUIRE(document.PrefabInstances()[1].sourcePrefab == PrefabAsset(7));
        REQUIRE(document.PrefabInstances()[1].parent == secondParent.Value().object);

        REQUIRE(commands.Execute(DeleteScenePrefabInstanceCommand{duplicateId}).HasValue());
        REQUIRE(document.PrefabInstances().size() == 1);
        const auto undone = commands.Undo();
        REQUIRE(undone.HasValue());
        REQUIRE(undone.Value().affectedPrefabInstances == std::vector{duplicateId});
        REQUIRE(document.PrefabInstances().size() == 2);
        REQUIRE(document.PrefabInstances()[1].instanceId == duplicateId);
        REQUIRE(document.PrefabInstances()[1].sourcePrefab == PrefabAsset(7));
        REQUIRE(commands.Redo().HasValue());
        REQUIRE(document.PrefabInstances().size() == 1);
    }

    TEST_CASE("Scene prefab root edits are undoable no-ops and honor containing-scene locks", "[unit][editor][prefab][history]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto parent = commands.Execute(CreateSceneObjectCommand{.name = "Parent"});
        REQUIRE(parent.HasValue());
        const auto created = commands.Execute(CreateScenePrefabInstanceCommand{PrefabAsset(), parent.Value().object, {}});
        REQUIRE(created.HasValue());
        const Prefab::PrefabInstanceId instance = *created.Value().prefabInstance;
        const Math::Transform moved{.translation = {4.0F, -2.0F, 9.0F}};

        const auto transformed = commands.Execute(SetScenePrefabInstanceRootTransformCommand{instance, moved});
        REQUIRE(transformed.HasValue());
        REQUIRE(transformed.Value().committed);
        REQUIRE(document.PrefabInstances().front().rootTransform == moved);
        REQUIRE(commands.Undo().HasValue());
        REQUIRE(document.PrefabInstances().front().rootTransform == Math::Transform{});
        REQUIRE(commands.Redo().HasValue());
        REQUIRE(document.PrefabInstances().front().rootTransform == moved);

        const DocumentRevision beforeNoOp = document.Revision();
        const auto noOp = commands.Execute(SetScenePrefabInstanceRootTransformCommand{instance, moved});
        REQUIRE(noOp.HasValue());
        REQUIRE_FALSE(noOp.Value().committed);
        REQUIRE(document.Revision() == beforeNoOp);

        REQUIRE(
            commands
                .Execute(SetSceneObjectEditorStateCommand{parent.Value().object, SceneObjectEditorState{.visible = true, .locked = true}})
                .HasValue());
        REQUIRE(commands.Execute(SetScenePrefabInstanceRootTransformCommand{instance, {}}).HasError());
        REQUIRE(commands.Execute(ReparentScenePrefabInstanceCommand{instance, std::nullopt}).HasError());
        REQUIRE(commands.Execute(DuplicateScenePrefabInstanceCommand{instance}).HasError());
        REQUIRE(commands.Execute(DeleteScenePrefabInstanceCommand{instance}).HasError());
        REQUIRE(document.PrefabInstances().size() == 1);
        REQUIRE(document.PrefabInstances().front().rootTransform == moved);
    }

    TEST_CASE("Deleting a containing-scene subtree removes attached prefab roots atomically", "[unit][editor][prefab]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto root = commands.Execute(CreateSceneObjectCommand{.name = "Root"});
        const auto child = commands.Execute(CreateSceneObjectCommand{.name = "Child", .parent = root.Value().object});
        REQUIRE(root.HasValue());
        REQUIRE(child.HasValue());
        const auto attached = commands.Execute(CreateScenePrefabInstanceCommand{PrefabAsset(), child.Value().object, {}});
        const auto independent = commands.Execute(CreateScenePrefabInstanceCommand{PrefabAsset(2), std::nullopt, {}});
        REQUIRE(attached.HasValue());
        REQUIRE(independent.HasValue());

        const auto deleted = commands.Execute(DeleteSceneObjectCommand{root.Value().object});
        REQUIRE(deleted.HasValue());
        REQUIRE(deleted.Value().affectedPrefabInstances == std::vector{*attached.Value().prefabInstance});
        REQUIRE(document.PrefabInstances().size() == 1);
        REQUIRE(document.PrefabInstances().front().instanceId == *independent.Value().prefabInstance);
        REQUIRE(commands.Undo().HasValue());
        REQUIRE(document.PrefabInstances().size() == 2);
        REQUIRE(document.PrefabInstances().front().instanceId == *attached.Value().prefabInstance);
    }

    TEST_CASE("Malformed prefab placements reject transactionally and cannot target prefab-local members",
              "[unit][editor][prefab][malformed]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const DocumentRevision initialRevision = document.Revision();
        REQUIRE(commands.Execute(CreateScenePrefabInstanceCommand{{}, std::nullopt, {}}).HasError());
        REQUIRE(commands.Execute(CreateScenePrefabInstanceCommand{PrefabAsset(), SceneObjectId{99}, {}}).HasError());
        Math::Transform nonFinite;
        nonFinite.translation.x = std::numeric_limits<float>::infinity();
        REQUIRE(commands.Execute(CreateScenePrefabInstanceCommand{PrefabAsset(), std::nullopt, nonFinite}).HasError());
        REQUIRE(document.Revision() == initialRevision);
        REQUIRE(document.PrefabInstances().empty());

        const auto existing = commands.Execute(CreateScenePrefabInstanceCommand{PrefabAsset(), std::nullopt, {}});
        REQUIRE(existing.HasValue());
        const auto sourceIdentity = document.PrefabInstances().front().sourcePrefab;
        const auto missingInstance = Prefab::PrefabInstanceId::Create(99).Value();
        REQUIRE(commands.Execute(SetScenePrefabInstanceRootTransformCommand{missingInstance, {}}).HasError());
        REQUIRE(commands.Execute(DuplicateScenePrefabInstanceCommand{missingInstance}).HasError());
        REQUIRE(commands.Execute(ReparentScenePrefabInstanceCommand{missingInstance, std::nullopt}).HasError());
        REQUIRE(commands.Execute(DeleteScenePrefabInstanceCommand{missingInstance}).HasError());
        REQUIRE(commands.Execute(ReparentScenePrefabInstanceCommand{*existing.Value().prefabInstance, SceneObjectId{99}}).HasError());
        REQUIRE(commands.Execute(SetScenePrefabInstanceRootTransformCommand{*existing.Value().prefabInstance, nonFinite}).HasError());
        REQUIRE(document.PrefabInstances().front().sourcePrefab == sourceIdentity);
        REQUIRE_FALSE(document.PrefabInstances().front().parent.has_value());
    }

    TEST_CASE("Runtime conversion rejects unresolved prefab references instead of publishing a partial definition",
              "[unit][editor][prefab][runtime]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        REQUIRE(commands.Execute(CreateSceneObjectCommand{.name = "Authored Object"}).HasValue());
        REQUIRE(commands.Execute(CreateScenePrefabInstanceCommand{PrefabAsset(), std::nullopt, {}}).HasValue());

        const auto converted = ConvertSceneDocumentToRuntime(document.Snapshot(), Runtime::SceneDefinitionId{1});
        REQUIRE(converted.HasError());
        REQUIRE(converted.ErrorValue().code.Value() == "scene_conversion.prefab_resolution_required");
    }

    TEST_CASE("Loading prefab placements validates identity parent and lifecycle counters transactionally",
              "[unit][editor][prefab][lifecycle]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        REQUIRE(document
                    .LoadSaved({SceneObjectSnapshot{.id = SceneObjectId{4}, .name = "Parent"}},
                               {ScenePrefabInstance{Prefab::PrefabInstanceId::Create(8).Value(), PrefabAsset(3), SceneObjectId{4}, {}}})
                    .HasValue());
        REQUIRE(document.PrefabInstances().front().instanceId == Prefab::PrefabInstanceId::Create(8).Value());

        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto next = commands.Execute(CreateScenePrefabInstanceCommand{PrefabAsset(4), std::nullopt, {}});
        REQUIRE(next.HasValue());
        REQUIRE(next.Value().prefabInstance == Prefab::PrefabInstanceId::Create(9).Value());

        const SceneDocumentSnapshot before = document.Snapshot();
        const auto duplicateId = Prefab::PrefabInstanceId::Create(8).Value();
        REQUIRE(document
                    .LoadSaved(before.objects, {ScenePrefabInstance{duplicateId, PrefabAsset(), std::nullopt, {}},
                                                ScenePrefabInstance{duplicateId, PrefabAsset(2), std::nullopt, {}}})
                    .HasError());
        REQUIRE(document.Snapshot().prefabInstances == before.prefabInstances);
        REQUIRE(document
                    .LoadSaved(before.objects,
                               {ScenePrefabInstance{Prefab::PrefabInstanceId::Create(10).Value(), PrefabAsset(), SceneObjectId{999}, {}}})
                    .HasError());
        REQUIRE(document.Snapshot().prefabInstances == before.prefabInstances);
    }

    TEST_CASE("Runtime conversion expands prefab instances from one immutable snapshot transactionally",
              "[unit][editor][prefab][runtime]") {
        using namespace Horo;
        using namespace Horo::Editor;
        const Assets::AssetId prefab = PrefabAsset(12).Asset();
        const auto resolver =
            ScenePrefabResolver(prefab, {{.localId = {}, .name = "Prefab Root"},
                                         {.localId = {4}, .parentLocalId = Prefab::LocalObjectId{}, .name = "Prefab Child"}});
        const SceneDocumentSnapshot document{
            .revision = DocumentRevision{2},
            .state = DocumentStateId{3},
            .objects = {SceneObjectSnapshot{.id = SceneObjectId{9}, .name = "Containing Parent"}},
            .prefabInstances = {ScenePrefabInstance{Prefab::PrefabInstanceId::Create(7).Value(), PrefabAsset(12), SceneObjectId{9}, {}}},
        };

        const auto converted = ConvertSceneDocumentToRuntime(document, Runtime::SceneDefinitionId{4}, resolver, ScenePrefabLimits());
        REQUIRE(converted.HasValue());
        REQUIRE(converted.Value().Entities().size() == 3);
        REQUIRE(converted.Value().Entities()[1].parent == std::optional{Runtime::SceneObjectId{9}});
        REQUIRE(converted.Value().Entities()[2].parent == std::optional{converted.Value().Entities()[1].object});
        REQUIRE(document.prefabInstances.front().sourcePrefab == PrefabAsset(12));
    }

    TEST_CASE("Runtime conversion rejects prefab payloads without a typed runtime projection", "[unit][editor][prefab][runtime]") {
        using namespace Horo;
        using namespace Horo::Editor;
        const Assets::AssetId prefab = PrefabAsset(13).Asset();
        const auto componentType = Gameplay::ComponentTypeId::Parse("game.tests.opaque_component").Value();
        const auto resolver =
            ScenePrefabResolver(prefab, {{.localId = {},
                                          .name = "Opaque root",
                                          .components = {{.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
                                                          .component = {.typeId = componentType}}}}});
        const SceneDocumentSnapshot document{
            .revision = {},
            .state = DocumentStateId{1},
            .prefabInstances = {ScenePrefabInstance{Prefab::PrefabInstanceId::Create(10).Value(), PrefabAsset(13), std::nullopt, {}}},
        };

        const auto converted = ConvertSceneDocumentToRuntime(document, Runtime::SceneDefinitionId{7}, resolver, ScenePrefabLimits());
        REQUIRE(converted.HasError());
        REQUIRE(converted.ErrorValue().code.Value() == "scene_conversion.prefab_component_projection_unsupported");
    }

    TEST_CASE("Runtime conversion projects provider-neutral prefab navigation agents", "[unit][editor][prefab][runtime][navigation]") {
        using namespace Horo;
        using namespace Horo::Editor;
        const Assets::AssetId prefab = PrefabAsset(14).Asset();
        const auto componentType = Gameplay::ComponentTypeId::Parse("game.horo.navigation_agent").Value();
        const Prefab::RawComponentPayload payload{
            .instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
            .component =
                {.typeId = componentType,
                 .schemaVersion = 1,
                 .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                 .payload = Bytes(
                     R"({"schemaVersion":1,"profile":7,"filter":9,"radiusOverride":0.75,"movementCapability":"grounded","enabled":true})")},
        };
        const auto resolver = ScenePrefabResolver(prefab, {{.localId = {}, .name = "Agent Root", .components = {payload}}});
        const SceneDocumentSnapshot document{
            .revision = {},
            .state = DocumentStateId{1},
            .prefabInstances = {ScenePrefabInstance{Prefab::PrefabInstanceId::Create(11).Value(), PrefabAsset(14), std::nullopt, {}}},
        };

        const auto converted = ConvertSceneDocumentToRuntime(document, Runtime::SceneDefinitionId{8}, resolver, ScenePrefabLimits());
        REQUIRE(converted.HasValue());
        REQUIRE(converted.Value().Entities().size() == 1);
        REQUIRE(converted.Value().Entities().front().components.navigationAgent.has_value());
        REQUIRE(converted.Value().Entities().front().components.navigationAgent->profile ==
                Navigation::NavigationAgentProfileId::Create(7).Value());
        REQUIRE(converted.Value().Entities().front().components.navigationAgent->radiusOverride == 0.75F);
    }

    TEST_CASE("Runtime conversion rejects cyclic prefab dependency expansion", "[unit][editor][prefab][runtime][malformed]") {
        using namespace Horo;
        using namespace Horo::Editor;
        const Assets::AssetId first = PrefabAsset(15).Asset();
        const Assets::AssetId second = PrefabAsset(16).Asset();
        const Prefab::PrefabSourceRevision firstRevision = ScenePrefabRevision(15);
        const Prefab::PrefabSourceRevision secondRevision = ScenePrefabRevision(16);

        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({ScenePrefabRecord(first, "cycle-first"), ScenePrefabRecord(second, "cycle-second")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);

        Prefab::PrefabComposition firstComposition{
            .nestedPlacements = {{.placementLocalId = {1}, .sourcePrefab = PrefabAsset(16), .authoredAgainst = secondRevision}},
        };
        Prefab::PrefabComposition secondComposition{
            .nestedPlacements = {{.placementLocalId = {2}, .sourcePrefab = PrefabAsset(15), .authoredAgainst = firstRevision}},
        };
        const auto resolver =
            Prefab::BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                      {{Prefab::PrefabDocument::Create({.projectVersion = ScenePrefabVersion(),
                                                                                        .assetId = first,
                                                                                        .objects = {{.localId = {}, .name = "First root"}},
                                                                                        .composition = std::move(firstComposition),
                                                                                        .referencedAssets = {second}},
                                                                                       ScenePrefabLimits())
                                                            .Value(),
                                                        firstRevision},
                                                       {Prefab::PrefabDocument::Create({.projectVersion = ScenePrefabVersion(),
                                                                                        .assetId = second,
                                                                                        .objects = {{.localId = {}, .name = "Second root"}},
                                                                                        .composition = std::move(secondComposition),
                                                                                        .referencedAssets = {first}},
                                                                                       ScenePrefabLimits())
                                                            .Value(),
                                                        secondRevision}},
                                                      ScenePrefabLimits())
                .Value();
        const SceneDocumentSnapshot document{
            .revision = {},
            .state = DocumentStateId{1},
            .prefabInstances = {ScenePrefabInstance{Prefab::PrefabInstanceId::Create(17).Value(), PrefabAsset(15), std::nullopt, {}}},
        };

        const auto converted = ConvertSceneDocumentToRuntime(document, Runtime::SceneDefinitionId{8}, resolver, ScenePrefabLimits());
        REQUIRE(converted.HasError());
        REQUIRE(converted.ErrorValue().code.Value() == Prefab::PrefabErrors::DependencyGraphInvalid.code.Value());
    }

    TEST_CASE("Broken prefab projection retains repairable authored identity and complete failure context",
              "[unit][editor][prefab][malformed]") {
        using namespace Horo;
        using namespace Horo::Editor;
        Assets::AssetId missing = Assets::AssetId::FromBytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 13});
        Assets::AssetRegistry registry;
        const auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {}, ScenePrefabLimits()).Value();
        const ScenePrefabInstance authored{Prefab::PrefabInstanceId::Create(8).Value(),
                                           Prefab::PrefabAssetReference::Create(missing).Value(),
                                           std::nullopt,
                                           {}};
        const SceneDocumentSnapshot document{.revision = {}, .state = DocumentStateId{1}, .prefabInstances = {authored}};

        const auto projection = BuildScenePrefabProjection(document, resolver, ScenePrefabLimits());
        REQUIRE(projection.HasValue());
        REQUIRE(projection.Value().HasBrokenInstances());
        REQUIRE(projection.Value().instances.front().IsBroken());
        REQUIRE(projection.Value().instances.front().authored == authored);
        REQUIRE(projection.Value().instances.front().failure->message.find("prefab instance 8") != std::string::npos);
        REQUIRE(projection.Value().instances.front().failure->message.find(missing.ToString()) != std::string::npos);

        const auto converted = ConvertSceneDocumentToRuntime(document, Runtime::SceneDefinitionId{5}, resolver, ScenePrefabLimits());
        REQUIRE(converted.HasError());
        REQUIRE(converted.ErrorValue().message.find("prefab instance 8") != std::string::npos);
    }

    TEST_CASE("Prefab runtime conversion rejects an over-budget required placement without a partial definition",
              "[unit][editor][prefab][boundary]") {
        using namespace Horo;
        using namespace Horo::Editor;
        const Assets::AssetId prefab = PrefabAsset(14).Asset();
        const auto resolver = ScenePrefabResolver(prefab, {{.localId = {}, .name = "Root"},
                                                           {.localId = {5}, .parentLocalId = Prefab::LocalObjectId{}, .name = "Child"}});
        Prefab::PrefabProjectPolicy policy;
        policy.maximumObjectCount = 1;
        const auto limits = ScenePrefabLimits(policy);
        const SceneDocumentSnapshot document{
            .revision = {},
            .state = DocumentStateId{1},
            .objects = {SceneObjectSnapshot{.id = SceneObjectId{1}, .name = "Authored"}},
            .prefabInstances = {ScenePrefabInstance{Prefab::PrefabInstanceId::Create(9).Value(), PrefabAsset(14), std::nullopt, {}}},
        };
        const auto projection = BuildScenePrefabProjection(document, resolver, limits);
        REQUIRE(projection.HasValue());
        REQUIRE(projection.Value().HasBrokenInstances());
        REQUIRE(projection.Value().instances.front().failure->code.Value() == Prefab::PrefabErrors::ObjectCountExceeded.code.Value());

        const auto converted = ConvertSceneDocumentToRuntime(document, Runtime::SceneDefinitionId{6}, resolver, limits);
        REQUIRE(converted.HasError());
        REQUIRE(converted.ErrorValue().code.Value() == Prefab::PrefabErrors::ObjectCountExceeded.code.Value());
    }
}  // namespace
