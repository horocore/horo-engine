#include "editor/document/SceneDocumentComparison.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct ComparisonIdentities {
        Prefab::PrefabInstanceId instanceOne;
        Prefab::PrefabInstanceId instanceTwo;
        Prefab::PrefabInstanceId instanceThree;
        Prefab::PrefabAssetReference documentSourceOne;
        Prefab::PrefabAssetReference documentSourceTwo;
        Prefab::PrefabAssetReference diskSourceOne;
        Prefab::PrefabAssetReference diskSourceThree;
    };

    [[nodiscard]] ComparisonIdentities MakeComparisonIdentities() {
        const auto instanceOne = Prefab::PrefabInstanceId::Create(1);
        const auto instanceTwo = Prefab::PrefabInstanceId::Create(2);
        const auto instanceThree = Prefab::PrefabInstanceId::Create(3);
        const auto documentSourceOne =
            Prefab::PrefabAssetReference::Create(Assets::AssetId::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa").Value());
        const auto documentSourceTwo =
            Prefab::PrefabAssetReference::Create(Assets::AssetId::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb").Value());
        const auto diskSourceOne =
            Prefab::PrefabAssetReference::Create(Assets::AssetId::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc").Value());
        const auto diskSourceThree =
            Prefab::PrefabAssetReference::Create(Assets::AssetId::Parse("dddddddd-dddd-4ddd-8ddd-dddddddddddd").Value());
        REQUIRE(instanceOne.HasValue());
        REQUIRE(instanceTwo.HasValue());
        REQUIRE(instanceThree.HasValue());
        REQUIRE(documentSourceOne.HasValue());
        REQUIRE(documentSourceTwo.HasValue());
        REQUIRE(diskSourceOne.HasValue());
        REQUIRE(diskSourceThree.HasValue());
        return {instanceOne.Value(),       instanceTwo.Value(),   instanceThree.Value(),  documentSourceOne.Value(),
                documentSourceTwo.Value(), diskSourceOne.Value(), diskSourceThree.Value()};
    }

    [[nodiscard]] SceneObjectSnapshot MakeDocumentModifiedObject() {
        return {
            .id = SceneObjectId{1},
            .name = "Document Name",
            .localTransform = Math::Transform{.translation = {1.0F, 0.0F, 0.0F}},
            .primitiveMesh = PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box),
        };
    }

    [[nodiscard]] SceneDocumentSnapshot MakeDocumentSnapshot(const ComparisonIdentities &ids, const SceneObjectSnapshot &documentModified) {
        return {
            .objects = {documentModified, SceneObjectSnapshot{.id = SceneObjectId{2}, .name = "Removed"}},
            .prefabInstances =
                {
                    ScenePrefabInstance{.instanceId = ids.instanceOne, .sourcePrefab = ids.documentSourceOne},
                    ScenePrefabInstance{.instanceId = ids.instanceTwo, .sourcePrefab = ids.documentSourceTwo},
                },
        };
    }

    [[nodiscard]] SceneDocumentSnapshot MakeDiskSnapshot(const ComparisonIdentities &ids, const SceneObjectSnapshot &diskModified) {
        return {
            .objects = {diskModified, SceneObjectSnapshot{.id = SceneObjectId{3}, .name = "Added"}},
            .prefabInstances =
                {
                    ScenePrefabInstance{.instanceId = ids.instanceOne, .sourcePrefab = ids.diskSourceOne},
                    ScenePrefabInstance{.instanceId = ids.instanceThree, .sourcePrefab = ids.diskSourceThree},
                },
        };
    }

    void RequireComparisonSummary(const SceneDocumentComparison &comparison) {
        REQUIRE(comparison.addedOnDisk == 1);
        REQUIRE(comparison.removedFromDisk == 1);
        REQUIRE(comparison.modified == 1);
        REQUIRE(comparison.prefabInstancesAddedOnDisk == 1);
        REQUIRE(comparison.prefabInstancesRemovedFromDisk == 1);
        REQUIRE(comparison.prefabInstancesModified == 1);
        REQUIRE(comparison.objects.size() == 3);
        REQUIRE(comparison.prefabInstances.size() == 3);
    }

    void RequireModifiedObjectComparison(const SceneDocumentComparison &comparison, const Prefab::PrefabInstanceId instanceOne) {
        const SceneObjectComparison &modified = comparison.objects[0];
        REQUIRE(modified.id == SceneObjectId{1});
        REQUIRE(modified.kind == SceneObjectComparisonKind::Modified);
        REQUIRE(modified.documentName == "Document Name");
        REQUIRE(modified.diskName == "Disk Name");
        REQUIRE(modified.fields.name);
        REQUIRE(modified.fields.transform);
        REQUIRE(modified.fields.components);
        REQUIRE(modified.fields.editorState);
        REQUIRE(!modified.fields.parent);
        REQUIRE(!modified.fields.primitive);
        REQUIRE(comparison.objects[1].kind == SceneObjectComparisonKind::RemovedFromDisk);
        REQUIRE(comparison.objects[2].kind == SceneObjectComparisonKind::AddedOnDisk);
        REQUIRE(comparison.prefabInstances[0].id == instanceOne);
        REQUIRE(comparison.prefabInstances[0].kind == SceneObjectComparisonKind::Modified);
        REQUIRE(comparison.prefabInstances[0].fields.sourcePrefab);
        REQUIRE(!comparison.prefabInstances[0].fields.parent);
        REQUIRE(!comparison.prefabInstances[0].fields.rootTransform);
        REQUIRE(comparison.prefabInstances[1].kind == SceneObjectComparisonKind::RemovedFromDisk);
        REQUIRE(comparison.prefabInstances[2].kind == SceneObjectComparisonKind::AddedOnDisk);
    }
}  // namespace

TEST_CASE("Scene Comparison Classifies Typed Added Removed And Modified Objects", "[unit][editor][persistence][compare]") {
    const ComparisonIdentities ids = MakeComparisonIdentities();
    const SceneObjectSnapshot documentModified = MakeDocumentModifiedObject();
    SceneObjectSnapshot diskModified = documentModified;
    diskModified.name = "Disk Name";
    diskModified.localTransform.translation = {2.0F, 0.0F, 0.0F};
    diskModified.components.light = Runtime::LightComponent{.kind = Runtime::LightKind::Point};
    diskModified.editorState.locked = true;

    const SceneDocumentComparison comparison =
        CompareSceneDocuments(MakeDocumentSnapshot(ids, documentModified), MakeDiskSnapshot(ids, diskModified));
    RequireComparisonSummary(comparison);
    RequireModifiedObjectComparison(comparison, ids.instanceOne);
}
