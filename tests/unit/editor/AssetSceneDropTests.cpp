#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/AssetSceneDrop.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct ExtractedScene {
        Runtime::PrimitiveMeshCache cache;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        std::unique_ptr<Runtime::RuntimeScene> runtime;
        EditorViewportSceneSnapshot snapshot;

        explicit ExtractedScene(const bool withSurface, const EditorViewportCamera camera = {}) {
            if (withSurface) {
                REQUIRE(
                    (commands.Execute(CreateSceneObjectCommand{.name = "Surface", .primitiveMesh = PrimitiveMeshDescriptor{}}).HasValue()));
            }
            auto definition = ConvertSceneDocumentToRuntime(document.Snapshot(), Runtime::SceneDefinitionId{1});
            REQUIRE((definition.HasValue()));
            auto created = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{1});
            REQUIRE((created.HasValue()));
            runtime = std::move(created).Value();
            auto extracted = ExtractEditorViewportScene(runtime->View(), document.Revision(), camera, cache);
            REQUIRE((extracted.HasValue()));
            snapshot = std::move(extracted).Value();
        }
    };

    TEST_CASE("Asset scene drop policy accepts registered core meshes and prefabs", "[unit][editor][asset-drop]") {
        const std::string id = "00112233-4455-6677-8899-aabbccddeeff";
        const AssetSceneDragPayload mesh = MakeAssetSceneDragPayload(id, "core.mesh", "/tmp/cube.mesh", true);
        CHECK(EvaluateAssetSceneDrop(mesh).canInstantiate);
        CHECK(EvaluateAssetSceneDrop(MakeAssetSceneDragPayload(id, "core.prefab", "/tmp/cube.prefab", true)).canInstantiate);
        CHECK_FALSE(EvaluateAssetSceneDrop(MakeAssetSceneDragPayload(id, "core.prefab", std::string(1200, 'x'), true)).canInstantiate);

        const AssetSceneDragPayload texture = MakeAssetSceneDragPayload(id, "core.texture", "/tmp/cube.png", true);
        CHECK_FALSE(EvaluateAssetSceneDrop(texture).canInstantiate);
        CHECK(EvaluateAssetSceneDrop(texture).rejection == AssetSceneDropRejection::UnsupportedType);

        const AssetSceneDragPayload unregistered = MakeAssetSceneDragPayload({}, "core.mesh", "/tmp/cube.mesh", false);
        CHECK_FALSE(EvaluateAssetSceneDrop(unregistered).canInstantiate);
        CHECK(EvaluateAssetSceneDrop(unregistered).rejection == AssetSceneDropRejection::Unregistered);

        AssetSceneDragPayload malformed = mesh;
        malformed.assetType.fill('x');
        CHECK(EvaluateAssetSceneDrop(malformed).rejection == AssetSceneDropRejection::InvalidPayload);
    }

    TEST_CASE("Hierarchy asset drop resolves row center as child and edges as siblings", "[unit][editor][asset-drop]") {
        const SceneObjectId hovered{7};
        const SceneObjectId parent{3};

        const HierarchyAssetDropPlacement before = ResolveHierarchyAssetDropPlacement(0.1F, hovered, parent);
        CHECK(before.zone == HierarchyAssetDropZone::BeforeSibling);
        CHECK(before.target == AssetSceneDropTarget::HierarchySibling);
        CHECK(before.parent == parent);

        const HierarchyAssetDropPlacement child = ResolveHierarchyAssetDropPlacement(0.5F, hovered, parent);
        CHECK(child.zone == HierarchyAssetDropZone::Child);
        CHECK(child.target == AssetSceneDropTarget::HierarchyChild);
        CHECK(child.parent == hovered);

        const HierarchyAssetDropPlacement rootSibling = ResolveHierarchyAssetDropPlacement(0.9F, hovered, std::nullopt);
        CHECK(rootSibling.zone == HierarchyAssetDropZone::AfterSibling);
        CHECK(rootSibling.target == AssetSceneDropTarget::HierarchySibling);
        CHECK_FALSE(rootSibling.parent.has_value());
    }

    TEST_CASE("Asset placement preview is transient tinted and non-pickable", "[unit][editor][asset-drop]") {
        Render::MeshData mesh;
        mesh.vertices = {
            {{-0.5F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}},
            {{0.5F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}},
            {{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}},
        };
        mesh.indices = {0, 1, 2};
        mesh.localBounds = {{-0.5F, 0.0F, 0.0F}, {0.5F, 1.0F, 0.0F}};
        const EditorAssetMeshView meshView{.handle = {{91}, 1}, .mesh = &mesh};
        EditorViewportSceneSnapshot scene;

        REQUIRE(
            (ApplyAssetViewportPlacementPreview(scene, meshView, AssetViewportPlacement{.worldPosition = {2.0F, 3.0F, 4.0F}}).HasValue()));
        REQUIRE((scene.meshResources.size() == 1));
        REQUIRE((scene.instances.size() == 1));
        CHECK(scene.instances.front().presentation.tintStrength > 0.0F);
        CHECK(scene.instancePickable == std::vector<std::uint8_t>{0U});
        CHECK(Math::TransformPoint(scene.instances.front().localToWorld, {}) == Math::Vec3{2.0F, 3.0F, 4.0F});

        const auto retained = scene;
        REQUIRE(ClearAssetViewportPlacementPreview(scene));
        CHECK(scene.meshLeases.empty());
        // Imported mesh storage is borrowed from meshView; primitive-cache leases are not its owner.
        CHECK(retained.meshLeases.empty());
        CHECK(retained.View().IsValid());
        CHECK(scene.instances.empty());
        CHECK(scene.instanceObjects.empty());
        CHECK(scene.instancePickable.empty());
        CHECK(scene.meshResources.empty());
        CHECK_FALSE(ClearAssetViewportPlacementPreview(scene));
    }

    TEST_CASE("Viewport asset placement prefers surface then ground plane then camera front", "[unit][editor][asset-drop]") {
        SECTION("surface hit") {
            ExtractedScene scene{true};
            auto placed = ResolveAssetViewportPlacement(AssetViewportPlacementRequest{
                .scene = scene.snapshot,
                .normalizedX = 0.5F,
                .normalizedY = 0.5F,
                .localBounds = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}},
            });
            REQUIRE((placed.HasValue()));
            CHECK(placed.Value().kind == AssetViewportPlacementKind::Surface);
            CHECK(Math::NearlyEqual(placed.Value().worldPosition.y, 0.5F));
        }

        SECTION("ground plane") {
            const EditorViewportCamera camera{.position = {0.0F, 4.0F, 4.0F}, .target = {0.0F, 0.0F, 0.0F}};
            ExtractedScene scene{false, camera};
            auto placed = ResolveAssetViewportPlacement(AssetViewportPlacementRequest{
                .scene = scene.snapshot,
                .normalizedX = 0.5F,
                .normalizedY = 0.5F,
                .localBounds = {{-1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, 1.0F}},
            });
            REQUIRE((placed.HasValue()));
            CHECK(placed.Value().kind == AssetViewportPlacementKind::GroundPlane);
            CHECK(Math::NearlyEqual(placed.Value().worldPosition.y, 1.0F));
        }

        SECTION("camera-front fallback and snapping") {
            ExtractedScene scene{false};
            auto placed = ResolveAssetViewportPlacement(AssetViewportPlacementRequest{
                .scene = scene.snapshot,
                .normalizedX = 0.5F,
                .normalizedY = 0.5F,
                .localBounds = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}},
                .snapToGrid = true,
                .gridStep = 1.0F,
            });
            REQUIRE((placed.HasValue()));
            CHECK(placed.Value().kind == AssetViewportPlacementKind::CameraFront);
            CHECK(Math::NearlyEqual(placed.Value().worldPosition.x, 0.0F));
            CHECK(Math::NearlyEqual(placed.Value().worldPosition.y, 1.0F));
            CHECK(Math::NearlyEqual(placed.Value().worldPosition.z, -1.0F));
        }
    }

    TEST_CASE("Asset instantiation is one undoable command with sibling naming and parenting", "[unit][editor][asset-drop]") {
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        InstantiateSceneAssetUseCase instantiate{document, commands};
        const auto asset = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff");
        REQUIRE((asset.HasValue()));
        const auto parent = commands.Execute(CreateSceneObjectCommand{.name = "Parent"});
        REQUIRE((parent.HasValue()));

        const auto first = instantiate.Execute({asset.Value(), "Chair", parent.Value().object, {}});
        const auto second =
            instantiate.Execute({asset.Value(), "Chair", parent.Value().object, Math::Transform{.translation = {1.0F, 2.0F, 3.0F}}});
        REQUIRE((first.HasValue() && second.HasValue()));
        REQUIRE((document.Objects().size() == 3));
        CHECK(document.Objects()[1].name == "Chair");
        CHECK(document.Objects()[2].name == "Chair 2");
        CHECK(document.Objects()[2].parent == parent.Value().object);
        CHECK(document.Objects()[2].meshAsset == asset.Value());
        CHECK(document.IsDirty());

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((document.Objects().size() == 2));
        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((document.Objects().size() == 3));
        CHECK(document.Objects().back().localTransform.translation == Math::Vec3{1.0F, 2.0F, 3.0F});
    }

    TEST_CASE("Prefab drop geometry composes nested transforms and clears every ghost resource", "[unit][editor][asset-drop]") {
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{8}, Runtime::SceneDefinitionRevision{1}};
        builder.Add({.object = Runtime::SceneObjectId{1},
                     .localTransform = {.translation = {2.0F, 0.0F, 0.0F}},
                     .primitiveMesh = Runtime::PrimitiveMeshDescriptor{}});
        builder.Add({.object = Runtime::SceneObjectId{2},
                     .parent = Runtime::SceneObjectId{1},
                     .localTransform = {.translation = {3.0F, 2.0F, 0.0F}, .scale = {2.0F, 2.0F, 2.0F}},
                     .primitiveMesh = Runtime::PrimitiveMeshDescriptor{}});
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        EditorViewportSceneSnapshot retained;
        {
            Runtime::PrimitiveMeshCache meshes;
            auto geometry = PreparePrefabDropGeometry(definition.Value(), meshes);
            REQUIRE(geometry.HasValue());
            REQUIRE(geometry.Value().instances.size() == 2);
            CHECK(geometry.Value().bounds.minimum.x == 1.5F);
            CHECK(geometry.Value().bounds.maximum.x == 6.0F);
            CHECK(geometry.Value().bounds.maximum.y == 3.0F);
            EditorViewportSceneSnapshot scene;
            const Math::Transform placement{.translation = {10.0F, 0.0F, 4.0F}};
            REQUIRE(ApplyPrefabDropPreview(scene, geometry.Value(), placement).HasValue());
            REQUIRE(scene.instances.size() == 2);
            CHECK(scene.instances.back().localToWorld.At(0, 3) == 15.0F);
            CHECK(scene.instancePickable == std::vector<std::uint8_t>{0U, 0U});
            retained = scene;
            REQUIRE(ClearAssetViewportPlacementPreview(scene));
            CHECK(scene.meshLeases.empty());
            CHECK(scene.instances.empty());
            CHECK(scene.meshResources.empty());
            CHECK_FALSE(ClearAssetViewportPlacementPreview(scene));
        }
        // The cache, prepared geometry and original preview have retired; the copied snapshot pins every resource it borrows.
        REQUIRE_FALSE(retained.meshLeases.empty());
        CHECK(retained.meshLeases.front().Data().IsValid());
        CHECK(retained.View().IsValid());
    }

    TEST_CASE("Prefab ghosts apply rotated nonuniform parent and placement transforms in world order", "[unit][editor][asset-drop]") {
        const auto quarterTurn = Math::Quaternion::FromAxisAngle({0.0F, 0.0F, 1.0F}, 1.5707963267948966F);
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{9}, Runtime::SceneDefinitionRevision{1}};
        builder.Add({.object = Runtime::SceneObjectId{1},
                     .localTransform = {.translation = {2.0F, 0.0F, 0.0F}, .rotation = quarterTurn, .scale = {2.0F, 3.0F, 1.0F}},
                     .primitiveMesh = Runtime::PrimitiveMeshDescriptor{}});
        builder.Add({.object = Runtime::SceneObjectId{2},
                     .parent = Runtime::SceneObjectId{1},
                     .localTransform = {.translation = {3.0F, 2.0F, 0.0F}, .scale = {2.0F, 1.0F, 1.0F}},
                     .primitiveMesh = Runtime::PrimitiveMeshDescriptor{}});
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        Runtime::PrimitiveMeshCache meshes;
        auto geometry = PreparePrefabDropGeometry(definition.Value(), meshes);
        REQUIRE(geometry.HasValue());
        REQUIRE(geometry.Value().instances.size() == 2);
        const auto localPoint = Math::TransformAffinePoint(geometry.Value().instances.back().localToWorld, {1.0F, 1.0F, 0.0F});
        CHECK(std::fabs(localPoint.x + 7.0F) < 0.0001F);
        CHECK(std::fabs(localPoint.y - 10.0F) < 0.0001F);
        CHECK(std::fabs(geometry.Value().bounds.minimum.x + 5.5F) < 0.0001F);
        CHECK(std::fabs(geometry.Value().bounds.maximum.y - 8.0F) < 0.0001F);
        EditorViewportSceneSnapshot scene;
        const Math::Transform placement{.translation = {10.0F, 1.0F, 4.0F}, .rotation = quarterTurn, .scale = {1.0F, 2.0F, 1.0F}};
        REQUIRE(ApplyPrefabDropPreview(scene, geometry.Value(), placement).HasValue());
        REQUIRE(scene.instances.size() == 2);
        const auto worldPoint = Math::TransformAffinePoint(scene.instances.back().localToWorld, {1.0F, 1.0F, 0.0F});
        // Independent expected coordinates detect reversing either parent/local or placement/world composition.
        CHECK(std::fabs(worldPoint.x + 10.0F) < 0.0001F);
        CHECK(std::fabs(worldPoint.y + 6.0F) < 0.0001F);
        CHECK(std::fabs(worldPoint.z - 4.0F) < 0.0001F);
    }
}  // namespace
