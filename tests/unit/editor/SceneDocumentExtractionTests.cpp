#include "SceneDocumentTestSupport.h"
#include "editor/renderer/xr/EditorXrSceneGeometry.h"
#include "editor/renderer/xr/EditorXrTrackingRoot.h"

#include <thread>

namespace {
    using namespace Horo::Editor::SceneDocumentTestSupport;

    struct HierarchySceneObjects {
        Horo::Editor::SceneObjectId parent;
        Horo::Editor::SceneObjectId child;
    };

    [[nodiscard]] HierarchySceneObjects MakeHierarchyScene(Horo::Editor::SceneDocument &document) {
        Horo::Editor::EditorHistory history;
        Horo::Editor::SceneDocumentCommandExecutor commands{document, history};
        const auto parent = commands.Execute(Horo::Editor::CreateSceneObjectCommand{
            .name = "Parent",
            .localTransform = Horo::Math::Transform{.translation = {2.0F, 0.0F, 0.0F}},
        });
        REQUIRE((parent.HasValue()));
        const auto child = commands.Execute(Horo::Editor::CreateSceneObjectCommand{
            .name = "Box",
            .parent = parent.Value().object,
            .localTransform = Horo::Math::Transform{.translation = {0.0F, 3.0F, 0.0F}},
            .primitiveMesh = Horo::Runtime::PrimitiveMeshDescriptor{},
        });
        REQUIRE((child.HasValue()));
        return {parent.Value().object, child.Value().object};
    }

    struct LightSceneObjects {
        Horo::Editor::SceneObjectId parent;
        Horo::Editor::SceneObjectId light;
        Horo::Runtime::LightComponent component;
    };

    [[nodiscard]] LightSceneObjects MakeLightScene(Horo::Editor::SceneDocument &document) {
        Horo::Editor::EditorHistory history;
        Horo::Editor::SceneDocumentCommandExecutor commands{document, history};
        const auto parent = commands.Execute(Horo::Editor::CreateSceneObjectCommand{
            .name = "Light Rig",
            .localTransform = Horo::Math::Transform{.translation = {2.0F, 0.0F, 0.0F}},
        });
        REQUIRE((parent.HasValue()));
        Horo::Editor::SceneObjectComponentSet components;
        components.light = Horo::Runtime::LightComponent{
            .kind = Horo::Runtime::LightKind::Spot,
            .color = {0.5F, 0.75F, 1.0F},
            .intensity = 3.0F,
            .range = 24.0F,
            .innerConeRadians = 0.25F,
            .outerConeRadians = 0.75F,
        };
        const auto lightObject = commands.Execute(Horo::Editor::CreateSceneObjectCommand{
            .name = "Spot",
            .parent = parent.Value().object,
            .localTransform =
                Horo::Math::Transform{
                    .translation = {0.0F, 3.0F, 0.0F},
                    .rotation = Horo::Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, Horo::Math::Pi * 0.5F),
                },
            .components = components,
        });
        REQUIRE((lightObject.HasValue()));
        return {parent.Value().object, lightObject.Value().object, *components.light};
    }

    TEST_CASE("XR camera-free geometry pins the canonical meshes and rejects stale workspace or candidate mappings", "[unit][editor][xr]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        const auto objects = MakeHierarchyScene(document);
        Runtime::PrimitiveMeshCache primitives;
        EditorAssetMeshCache assets;
        auto owner = std::make_unique<EditorXrSceneSourceOwner>(document, primitives, assets);
        const auto port = owner->Port();
        auto scene = MakeRuntimeScene(document);
        const auto revision = scene->View().DefinitionRevision();
        REQUIRE(revision.value == document.State().value);
        REQUIRE(revision.value != document.Revision().value);
        REQUIRE(port.Capture({document.Revision().value}, scene->View()).HasError());
        REQUIRE(port.Capture({revision.value + 1}, scene->View()).HasError());
        auto captured = port.Capture(revision, scene->View());
        REQUIRE(captured.HasValue());
        auto prepared = std::move(captured).Value();
        REQUIRE(prepared->Resources().size() == 1);
        auto instances = prepared->Update(scene->View());
        REQUIRE(instances.HasValue());
        REQUIRE(instances.Value().size() == 1);
        const auto origin = Math::TransformPoint(instances.Value()[0].localToWorld, {});
        CHECK(NearlyEqual(origin.x, 2.0F));
        CHECK(NearlyEqual(origin.y, 3.0F));
        const auto desktop = ExtractEditorViewportScene(scene->View(), document.Revision(), {}, primitives);
        REQUIRE(desktop.HasValue());
        CHECK(desktop.Value().instances[0].mesh == instances.Value()[0].mesh);
        CHECK(desktop.Value().instanceObjects[0] == objects.child);
        CHECK(prepared->Resources()[0].vertices.data() == desktop.Value().meshResources[0].vertices.data());
        SECTION("foreign Scene occurrence cannot reuse the authored IDs") {
            auto definition = ConvertSceneDocumentToRuntime(document.Snapshot(), Runtime::SceneDefinitionId{1});
            REQUIRE(definition.HasValue());
            auto other = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{2});
            REQUIRE(other.HasValue());
            CHECK(prepared->Update(other.Value()->View()).HasError());
        }
        SECTION("source edit revokes before candidate access") {
            EditorHistory history;
            SceneDocumentCommandExecutor commands(document, history);
            REQUIRE(commands.Execute(CreateSceneObjectCommand{.name = "New source revision"}).HasValue());
            CHECK(prepared->ValidateSource().HasError());
            CHECK(prepared->Update(scene->View()).HasError());
        }
        SECTION("cache reload retires the prior source incarnation without dropping its owned payload") {
            const auto *vertices = prepared->Resources()[0].vertices.data();
            assets.Clear();
            CHECK(prepared->ValidateSource().HasError());
            CHECK(prepared->Resources()[0].vertices.data() == vertices);
        }
        SECTION("explicit cancellation does not discard queued mesh leases") {
            prepared->Revoke();
            CHECK(prepared->Update(scene->View()).HasError());
            CHECK(prepared->Resources()[0].IsValid());
        }
        SECTION("wrong lane rejects without borrowing document or caches") {
            bool rejected = false;
            std::thread worker([&] {
                rejected = prepared->ValidateSource().HasError();
            });
            worker.join();
            CHECK(rejected);
        }
        SECTION("retired workspace cannot be revived by its weak port") {
            owner.reset();
            CHECK(prepared->ValidateSource().HasError());
            CHECK(port.Capture(revision, scene->View()).HasError());
            CHECK(prepared->Resources()[0].IsValid());
        }
    }

    TEST_CASE("XR strict geometry admission keeps the original desktop instance capacity", "[unit][editor][xr]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands(document, history);
        for (std::size_t index = 0; index <= PreparedEditorXrGeometry::MaximumInstances; ++index)
            REQUIRE(
                commands.Execute(CreateSceneObjectCommand{.name = "Box", .primitiveMesh = Runtime::PrimitiveMeshDescriptor{}}).HasValue());
        Runtime::PrimitiveMeshCache primitives;
        EditorAssetMeshCache assets;
        EditorXrSceneSourceOwner owner(document, primitives, assets);
        auto scene = MakeRuntimeScene(document);
        CHECK(owner.Port().Capture(scene->View().DefinitionRevision(), scene->View()).HasError());
        const auto desktop = ExtractEditorViewportScene(scene->View(), document.Revision(), {}, primitives);
        REQUIRE(desktop.HasValue());
        CHECK(desktop.Value().instances.size() == PreparedEditorXrGeometry::MaximumInstances + 1);
    }

    TEST_CASE("XR imported geometry absence is a strict error while desktop retains its prior omission policy", "[unit][editor][xr]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands(document, history);
        const auto asset = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff");
        REQUIRE(asset.HasValue());
        REQUIRE(commands.Execute(CreateSceneObjectCommand{.name = "Imported", .meshAsset = asset.Value()}).HasValue());
        Runtime::PrimitiveMeshCache primitives;
        EditorAssetMeshCache assets;
        EditorXrSceneSourceOwner owner(document, primitives, assets);
        auto scene = MakeRuntimeScene(document);
        CHECK(owner.Port().Capture(scene->View().DefinitionRevision(), scene->View()).HasError());
        const auto snapshot = document.Snapshot();
        const auto desktop = ExtractEditorViewportScene(scene->View(), document.Revision(), {}, primitives, &snapshot, &assets);
        REQUIRE(desktop.HasValue());
        CHECK(desktop.Value().instances.empty());
    }

    TEST_CASE("Extraction Resolves Hierarchy Into World Matrices", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        const auto objects = MakeHierarchyScene(document);
        Runtime::PrimitiveMeshCache meshCache;

        const auto runtimeScene = MakeRuntimeScene(document);
        const auto extracted = ExtractEditorViewportScene(runtimeScene->View(), document.Revision(), EditorViewportCamera{}, meshCache);
        REQUIRE((extracted.HasValue()));
        REQUIRE((extracted.Value().documentRevision == document.Revision()));
        REQUIRE((extracted.Value().instances.size() == 1));
        REQUIRE((extracted.Value().instanceObjects == std::vector{objects.child}));
        const Math::Vec3 worldOrigin = Math::TransformPoint(extracted.Value().instances.front().localToWorld, {});
        REQUIRE((NearlyEqual(worldOrigin.x, 2.0F)));
        REQUIRE((NearlyEqual(worldOrigin.y, 3.0F)));
        REQUIRE((NearlyEqual(worldOrigin.z, 0.0F)));
        const auto resolved = ResolveSceneObjectWorldTransforms(runtimeScene->View(), objects.child);
        REQUIRE((resolved.HasValue()));
        const Math::Vec3 resolvedOrigin = Math::TransformPoint(resolved.Value().localToWorld, {});
        const Math::Vec3 resolvedParentOrigin = Math::TransformPoint(resolved.Value().parentToWorld, {});
        REQUIRE((NearlyEqual(resolvedOrigin.x, 2.0F) && NearlyEqual(resolvedOrigin.y, 3.0F)));
        REQUIRE((NearlyEqual(resolvedParentOrigin.x, 2.0F) && NearlyEqual(resolvedParentOrigin.y, 0.0F)));
    }

    TEST_CASE("XR tracking root resolves exact authored rigid hierarchy and rejects missing or scaled placement", "[unit][editor][xr]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        const auto objects = MakeLightScene(document);
        auto scene = MakeRuntimeScene(document);
        const auto root = ResolveXrTrackingRoot(scene->View(), Runtime::SceneObjectId{objects.light.value});
        REQUIRE(root.HasValue());
        CHECK(NearlyEqual(root.Value().translation.x, 2.0F));
        CHECK(NearlyEqual(root.Value().translation.y, 3.0F));
        const auto direction = root.Value().rotation.Rotate({0.0F, 0.0F, -1.0F});
        CHECK(NearlyEqual(direction.x, -1.0F));
        CHECK(NearlyEqual(direction.z, 0.0F));
        CHECK(ResolveXrTrackingRoot(scene->View(), Runtime::SceneObjectId{}).HasError());
        CHECK(ResolveXrTrackingRoot(scene->View(), Runtime::SceneObjectId{999999}).HasError());
        const auto stale = scene->View();
        scene.reset();
        CHECK(ResolveXrTrackingRoot(stale, Runtime::SceneObjectId{objects.light.value}).HasError());

        EditorHistory history;
        SceneDocument scaledDocument;
        SceneDocumentCommandExecutor commands{scaledDocument, history};
        for (const auto scale : {Math::Vec3{1.0F, 2.0F, 1.0F}, Math::Vec3{-1.0F, 1.0F, 1.0F}}) {
            const auto created = commands.Execute(
                CreateSceneObjectCommand{.name = "Invalid tracking root", .localTransform = Math::Transform{.scale = scale}});
            REQUIRE(created.HasValue());
            auto scaled = MakeRuntimeScene(scaledDocument);
            CHECK(ResolveXrTrackingRoot(scaled->View(), Runtime::SceneObjectId{created.Value().object.value}).HasError());
        }
    }

    TEST_CASE("Extraction Applies Hierarchy Transform Previews", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        const auto objects = MakeHierarchyScene(document);
        Runtime::PrimitiveMeshCache meshCache;
        const auto runtimeScene = MakeRuntimeScene(document);
        const auto extracted = ExtractEditorViewportScene(runtimeScene->View(), document.Revision(), EditorViewportCamera{}, meshCache);
        REQUIRE((extracted.HasValue()));
        auto previewScene = extracted.Value();

        const SceneObjectTransformPreview preview{
            .object = objects.parent,
            .localTransform = Math::Transform{.translation = {5.0F, 0.0F, 0.0F}},
        };
        const std::array previews{preview};
        REQUIRE((ApplyEditorViewportTransformPreview(runtimeScene->View(), previews, previewScene).HasValue()));
        const Math::Vec3 previewOrigin = Math::TransformPoint(previewScene.instances.front().localToWorld, {});
        REQUIRE((NearlyEqual(previewOrigin.x, 5.0F) && NearlyEqual(previewOrigin.y, 3.0F)));
        REQUIRE((document.Revision() == extracted.Value().documentRevision));

        const std::array hierarchyPreviews{
            preview,
            SceneObjectTransformPreview{
                .object = objects.child,
                .localTransform = Math::Transform{.translation = {0.0F, 7.0F, 0.0F}},
            },
        };
        REQUIRE((ApplyEditorViewportTransformPreview(runtimeScene->View(), hierarchyPreviews, previewScene).HasValue()));
        const Math::Vec3 hierarchyPreviewOrigin = Math::TransformPoint(previewScene.instances.front().localToWorld, {});
        REQUIRE((NearlyEqual(hierarchyPreviewOrigin.x, 5.0F) && NearlyEqual(hierarchyPreviewOrigin.y, 7.0F)));

        const std::span<const SceneObjectTransformPreview> noPreviews;
        REQUIRE((ApplyEditorViewportTransformPreview(runtimeScene->View(), noPreviews, previewScene).HasValue()));
        const Math::Vec3 restoredOrigin = Math::TransformPoint(previewScene.instances.front().localToWorld, {});
        REQUIRE((NearlyEqual(restoredOrigin.x, 2.0F) && NearlyEqual(restoredOrigin.y, 3.0F)));
    }

    TEST_CASE("Extraction Produces World Space Lights", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        const auto objects = MakeLightScene(document);
        Runtime::PrimitiveMeshCache meshCache;

        const auto runtimeScene = MakeRuntimeScene(document);
        const auto extracted = ExtractEditorViewportScene(runtimeScene->View(), document.Revision(), EditorViewportCamera{}, meshCache);
        REQUIRE((extracted.HasValue()));
        REQUIRE((extracted.Value().lights.size() == 1));
        REQUIRE((extracted.Value().lightObjects == std::vector{objects.light}));
        const Render::RenderLight &light = extracted.Value().lights.front();
        REQUIRE((light.kind == Render::RenderLightKind::Spot));
        REQUIRE((Math::NearlyEqual(light.position, Math::Vec3{2.0F, 3.0F, 0.0F})));
        REQUIRE((NearlyEqual(light.innerConeCosine, std::cos(0.25F))));
        REQUIRE((NearlyEqual(light.outerConeCosine, std::cos(0.75F))));
        REQUIRE((light.IsValid()));
    }

    TEST_CASE("Extraction Applies World Space Light Previews", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        const auto objects = MakeLightScene(document);
        Runtime::PrimitiveMeshCache meshCache;
        const auto runtimeScene = MakeRuntimeScene(document);
        auto preview = ExtractEditorViewportScene(runtimeScene->View(), document.Revision(), EditorViewportCamera{}, meshCache).Value();

        const std::array previews{
            SceneObjectTransformPreview{
                .object = objects.parent,
                .localTransform = Math::Transform{.translation = {5.0F, 0.0F, 0.0F}},
            },
            SceneObjectTransformPreview{
                .object = objects.light,
                .localTransform = Math::Transform{.translation = {0.0F, 4.0F, 0.0F}},
            },
        };
        REQUIRE((ApplyEditorViewportTransformPreview(runtimeScene->View(), previews, preview).HasValue()));
        REQUIRE((Math::NearlyEqual(preview.lights.front().position, Math::Vec3{5.0F, 4.0F, 0.0F})));
        REQUIRE((Math::NearlyEqual(preview.lights.front().direction, Math::Vec3{0.0F, 0.0F, -1.0F})));

        Runtime::LightComponent previewComponent = objects.component;
        previewComponent.intensity = 9.0F;
        previewComponent.range = 40.0F;
        previewComponent.innerConeRadians = 0.4F;
        const SceneObjectLightPreview lightPreview{objects.light, previewComponent};
        REQUIRE((ApplyEditorViewportLightPreview(runtimeScene->View(), &lightPreview, preview).HasValue()));
        REQUIRE((NearlyEqual(preview.lights.front().intensity, 9.0F)));
        REQUIRE((NearlyEqual(preview.lights.front().range, 40.0F)));
        REQUIRE((NearlyEqual(preview.lights.front().innerConeCosine, std::cos(0.4F))));
        REQUIRE((ApplyEditorViewportLightPreview(runtimeScene->View(), nullptr, preview).HasValue()));
        REQUIRE((NearlyEqual(preview.lights.front().intensity, objects.component.intensity)));
    }

    TEST_CASE("Delete Removes Complete Subtree", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto parent = commands.Execute(CreateSceneObjectCommand{.name = "Parent"});
        REQUIRE((parent.HasValue()));
        REQUIRE((commands.Execute(CreateSceneObjectCommand{.name = "Child", .parent = parent.Value().object}).HasValue()));
        REQUIRE((commands.Execute(DeleteSceneObjectCommand{parent.Value().object}).HasValue()));
        REQUIRE((document.Snapshot().objects.empty()));
    }

    TEST_CASE("Batch Delete Normalizes Selected Ancestors And Restores Every Subtree Atomically", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        SceneObjectComponentSet parentComponents;
        parentComponents.camera = Horo::Runtime::CameraComponent{
            .projection = Horo::Runtime::CameraProjection::Orthographic,
            .orthographicHeight = 24.0F,
        };
        const auto parent = commands.Execute(CreateSceneObjectCommand{
            .name = "Parent",
            .localTransform = Horo::Math::Transform{.translation = {1.0F, 2.0F, 3.0F}},
            .components = parentComponents,
        });
        REQUIRE((parent.HasValue()));
        const auto child = commands.Execute(CreateSceneObjectCommand{
            .name = "Child",
            .parent = parent.Value().object,
            .localTransform = Horo::Math::Transform{.translation = {4.0F, 5.0F, 6.0F}},
            .primitiveMesh = PrimitiveMeshDescriptor{},
        });
        const auto independent = commands.Execute(CreateSceneObjectCommand{.name = "Independent"});
        const auto survivor = commands.Execute(CreateSceneObjectCommand{.name = "Survivor"});
        REQUIRE((child.HasValue() && independent.HasValue() && survivor.HasValue()));
        const SceneDocumentSnapshot before = document.Snapshot();
        const DocumentRevision revisionBeforeDelete = document.Revision();

        const auto deleted = commands.Execute(DeleteSceneObjectsCommand{
            {parent.Value().object, child.Value().object, independent.Value().object, parent.Value().object, SceneObjectId{999999}}});
        REQUIRE((deleted.HasValue()));
        REQUIRE((deleted.Value().affectedObjects.size() == 3));
        REQUIRE((document.Revision().value == revisionBeforeDelete.value + 1));
        REQUIRE((document.Objects().size() == 1));
        REQUIRE((document.Objects().front().id == survivor.Value().object));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((document.Objects().size() == before.objects.size()));
        for (std::size_t index = 0; index < before.objects.size(); ++index) {
            REQUIRE((document.Objects()[index].id == before.objects[index].id));
            REQUIRE((document.Objects()[index].parent == before.objects[index].parent));
            REQUIRE((document.Objects()[index].name == before.objects[index].name));
            REQUIRE((document.Objects()[index].localTransform == before.objects[index].localTransform));
            REQUIRE((document.Objects()[index].primitiveMesh == before.objects[index].primitiveMesh));
            REQUIRE((document.Objects()[index].components == before.objects[index].components));
            REQUIRE((document.Objects()[index].meshAsset == before.objects[index].meshAsset));
        }

        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((document.Objects().size() == 1));
        REQUIRE((document.Objects().front().id == survivor.Value().object));
    }

    TEST_CASE("Undo Redo Preserve Monotonic Revision And Saved State Identity", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Box"});
        REQUIRE((created.HasValue()));
        const DocumentStateId savedState = document.State();
        REQUIRE((document.MarkSaved(document.Revision(), savedState).HasValue()));

        REQUIRE((commands.Execute(RenameSceneObjectCommand{created.Value().object, "Renamed"}).HasValue()));
        const DocumentStateId renamedState = document.State();
        REQUIRE((document.Revision().value == 2));
        REQUIRE((document.IsDirty()));
        REQUIRE((history.CanUndo() && !history.CanRedo()));

        const auto undone = commands.Undo();
        REQUIRE((undone.HasValue() && undone.Value().kind == DocumentChangeKind::Undone));
        REQUIRE((document.Revision().value == 3));
        REQUIRE((document.State() == savedState));
        REQUIRE((!document.IsDirty()));
        REQUIRE((document.Snapshot().objects.front().name == "Box"));
        REQUIRE((history.CanRedo()));

        const auto redone = commands.Redo();
        REQUIRE((redone.HasValue() && redone.Value().kind == DocumentChangeKind::Redone));
        REQUIRE((document.Revision().value == 4));
        REQUIRE((document.State() == renamedState));
        REQUIRE((document.IsDirty()));
        REQUIRE((document.Snapshot().objects.front().name == "Renamed"));
    }

    TEST_CASE("Undo Delete Restores Subtree And New Edit Clears Redo", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto parent = commands.Execute(CreateSceneObjectCommand{.name = "Parent"});
        REQUIRE((parent.HasValue()));
        const auto child = commands.Execute(CreateSceneObjectCommand{.name = "Child", .parent = parent.Value().object});
        REQUIRE((child.HasValue()));
        REQUIRE((commands.Execute(DeleteSceneObjectCommand{parent.Value().object}).HasValue()));
        REQUIRE((document.Snapshot().objects.empty()));
        REQUIRE((commands.Undo().HasValue()));
        const SceneDocumentSnapshot restored = document.Snapshot();
        REQUIRE((restored.objects.size() == 2));
        REQUIRE((restored.objects[0].id == parent.Value().object));
        REQUIRE((restored.objects[1].parent == parent.Value().object));

        REQUIRE((commands.Execute(RenameSceneObjectCommand{child.Value().object, "Edited Child"}).HasValue()));
        REQUIRE((!history.CanRedo()));
        const auto redo = commands.Redo();
        REQUIRE((redo.HasError()));
        REQUIRE((redo.ErrorValue().code.Value() == "scene_document.nothing_to_redo"));
    }

    TEST_CASE("History Evicts Oldest Transactions Without Changing Current Document", "[unit][editor][history]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Initial"});
        REQUIRE((created.HasValue()));

        for (std::size_t index = 0; index < 257; ++index) {
            REQUIRE((commands.Execute(RenameSceneObjectCommand{created.Value().object, "Name " + std::to_string(index)}).HasValue()));
        }
        REQUIRE((document.Snapshot().objects.front().name == "Name 256"));

        for (std::size_t index = 0; index < 256; ++index) {
            REQUIRE((commands.Undo().HasValue()));
        }
        REQUIRE((document.Snapshot().objects.front().name == "Name 0"));
        const DocumentRevision revisionBeforeRejectedUndo = document.Revision();
        const auto exhausted = commands.Undo();
        REQUIRE((exhausted.HasError()));
        REQUIRE((exhausted.ErrorValue().code.Value() == "scene_document.nothing_to_undo"));
        REQUIRE((document.Revision() == revisionBeforeRejectedUndo));
        REQUIRE((document.Snapshot().objects.front().name == "Name 0"));
    }

    TEST_CASE("Extraction Uses All Primitive Meshes And Deduplicates Descriptors", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        constexpr std::array types{Runtime::PrimitiveMeshType::Box,     Runtime::PrimitiveMeshType::Sphere,
                                   Runtime::PrimitiveMeshType::Capsule, Runtime::PrimitiveMeshType::Cylinder,
                                   Runtime::PrimitiveMeshType::Cone,    Runtime::PrimitiveMeshType::Plane,
                                   Runtime::PrimitiveMeshType::Quad};
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        for (std::size_t index = 0; index < types.size(); ++index) {
            REQUIRE((commands
                         .Execute(CreateSceneObjectCommand{
                             .name = "Primitive " + std::to_string(index),
                             .primitiveMesh = Runtime::PrimitiveMeshDescriptor::Defaults(types[index]),
                         })
                         .HasValue()));
        }
        REQUIRE((commands
                     .Execute(CreateSceneObjectCommand{
                         .name = "Second Box",
                         .primitiveMesh = Runtime::PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box),
                     })
                     .HasValue()));
        Runtime::PrimitiveMeshCache meshCache;
        const auto runtimeScene = MakeRuntimeScene(document);
        const auto extracted = ExtractEditorViewportScene(runtimeScene->View(), document.Revision(), {}, meshCache);
        REQUIRE((extracted.HasValue()));
        REQUIRE((extracted.Value().instances.size() == 8));
        REQUIRE((extracted.Value().meshResources.size() == 7));
        REQUIRE((extracted.Value().meshLeases.size() == 7));
        REQUIRE((extracted.Value().instances.front().mesh == extracted.Value().instances.back().mesh));
        REQUIRE((extracted.Value().View().IsValid()));
    }

    TEST_CASE("Primitive Parameters Survive Snapshot Duplicate And History", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        Runtime::PrimitiveMeshDescriptor sphere = Runtime::PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Sphere);
        sphere.parameters = Runtime::SphereMeshParameters{.radius = 1.25F, .slices = 12, .stacks = 6};
        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Sphere", .primitiveMesh = sphere});
        REQUIRE((created.HasValue()));
        const auto duplicated = commands.Execute(DuplicateSceneObjectCommand{created.Value().object, "Sphere Copy"});
        REQUIRE((duplicated.HasValue()));
        SceneDocumentSnapshot snapshot = document.Snapshot();
        REQUIRE((snapshot.objects.size() == 2));
        REQUIRE((snapshot.objects[0].primitiveMesh == sphere));
        REQUIRE((snapshot.objects[1].primitiveMesh == sphere));
        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((document.Snapshot().objects.size() == 1));
        REQUIRE((commands.Redo().HasValue()));
        snapshot = document.Snapshot();
        REQUIRE((snapshot.objects.size() == 2 && snapshot.objects[1].primitiveMesh == sphere));
    }

    TEST_CASE("Behavior Attach Edit Remove Duplicate And History Preserve Stable Attachment Identity", "[unit][editor][gameplay]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Player"});
        REQUIRE(created.HasValue());
        const auto type = Gameplay::BehaviorTypeId::Parse("game.tests.player_controller");
        REQUIRE(type.HasValue());
        REQUIRE(commands
                    .Execute(AttachSceneObjectBehaviorCommand{
                        created.Value().object,
                        type.Value(),
                        1,
                        true,
                        false,
                        {Gameplay::BehaviorField{"speed", 2.0}},
                    })
                    .HasValue());
        REQUIRE(document.Objects().front().components.behaviors.size() == 1);
        const Gameplay::BehaviorInstanceId firstId = document.Objects().front().components.behaviors.front().instanceId;
        REQUIRE(firstId.IsValid());
        REQUIRE(commands.Execute(AttachSceneObjectBehaviorCommand{created.Value().object, type.Value()}).HasError());

        Gameplay::BehaviorComponent edited = document.Objects().front().components.behaviors.front();
        edited.enabled = false;
        edited.fields.front().value = 4.0;
        REQUIRE(commands.Execute(SetSceneObjectBehaviorCommand{created.Value().object, edited}).HasValue());
        REQUIRE(!document.Objects().front().components.behaviors.front().enabled);
        REQUIRE(commands.Undo().HasValue());
        REQUIRE(document.Objects().front().components.behaviors.front().enabled);
        REQUIRE(commands.Redo().HasValue());
        REQUIRE(!document.Objects().front().components.behaviors.front().enabled);

        const auto duplicate = commands.Execute(DuplicateSceneObjectCommand{created.Value().object, "Player Copy"});
        REQUIRE(duplicate.HasValue());
        REQUIRE(document.Objects().back().components.behaviors.front().instanceId != firstId);
        REQUIRE(commands.Execute(RemoveSceneObjectBehaviorCommand{created.Value().object, firstId}).HasValue());
        REQUIRE(document.Objects().front().components.behaviors.empty());
        REQUIRE(commands.Undo().HasValue());
        REQUIRE(document.Objects().front().components.behaviors.front().instanceId == firstId);
    }
}  // namespace
