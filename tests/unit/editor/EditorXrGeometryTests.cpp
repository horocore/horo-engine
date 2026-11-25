#include "SceneDocumentHierarchyFixture.h"
#include "editor/renderer/xr/EditorXrSceneGeometry.h"

#include <thread>
#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Editor::PreparedEditorXrGeometry::ConstructionKey>);
static_assert(!std::is_aggregate_v<Horo::Editor::PreparedEditorXrGeometry::ConstructionKey>);

namespace {
    using namespace Horo::Editor::SceneDocumentTestSupport;

    /** @brief Proves the real camera-free cold geometry admission before exercising source revocation paths. */
    std::unique_ptr<Horo::Editor::PreparedEditorXrGeometry> CaptureCameraFreeGeometry(const Horo::Editor::EditorXrSceneSourcePort &port,
                                                                                      Horo::Runtime::SceneDefinitionRevision revision,
                                                                                      Horo::Runtime::RuntimeSceneView scene,
                                                                                      const Horo::Editor::SceneDocument &document,
                                                                                      const HierarchySceneObjects &objects,
                                                                                      Horo::Runtime::PrimitiveMeshCache &primitives) {
        using namespace Horo;
        using namespace Horo::Editor;
        REQUIRE(revision.value == document.State().value);
        REQUIRE(revision.value != document.Revision().value);
        REQUIRE(port.Capture({document.Revision().value}, scene).HasError());
        REQUIRE(port.Capture({revision.value + 1}, scene).HasError());
        auto captured = port.Capture(revision, scene);
        REQUIRE(captured.HasValue());
        auto prepared = std::move(captured).Value();
        REQUIRE(prepared->Resources().size() == 1);
        auto instances = prepared->Update(scene);
        REQUIRE(instances.HasValue());
        REQUIRE(instances.Value().size() == 1);
        const auto origin = Math::TransformPoint(instances.Value()[0].localToWorld, {});
        CHECK(NearlyEqual(origin.x, 2.0F));
        CHECK(NearlyEqual(origin.y, 3.0F));
        const auto desktop = ExtractEditorViewportScene(scene, document.Revision(), {}, primitives);
        REQUIRE(desktop.HasValue());
        CHECK(desktop.Value().instances[0].mesh == instances.Value()[0].mesh);
        CHECK(desktop.Value().instanceObjects[0] == objects.child);
        CHECK(prepared->Resources()[0].vertices.data() == desktop.Value().meshResources[0].vertices.data());
        return prepared;
    }

    /** @brief Owns the canonical source, meshes and candidate until each independent revocation scenario completes. */
    struct XrGeometryFixture {
        Horo::Editor::SceneDocument document;
        const HierarchySceneObjects objects{MakeHierarchyScene(document)};
        Horo::Runtime::PrimitiveMeshCache primitives;
        Horo::Editor::EditorAssetMeshCache assets;
        std::unique_ptr<Horo::Editor::EditorXrSceneSourceOwner> owner{
            std::make_unique<Horo::Editor::EditorXrSceneSourceOwner>(document, primitives, assets)};
        const Horo::Editor::EditorXrSceneSourcePort port{owner->Port()};
        std::unique_ptr<Horo::Runtime::RuntimeScene> scene{MakeRuntimeScene(document)};
        const Horo::Runtime::SceneDefinitionRevision revision{scene->View().DefinitionRevision()};
        std::unique_ptr<Horo::Editor::PreparedEditorXrGeometry> prepared{
            CaptureCameraFreeGeometry(port, revision, scene->View(), document, objects, primitives)};
    };

    TEST_CASE_METHOD(XrGeometryFixture,
                     "XR camera-free geometry pins the canonical meshes and rejects stale workspace or candidate mappings",
                     "[unit][editor][xr]") {
        using namespace Horo;
        using namespace Horo::Editor;
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

    TEST_CASE_METHOD(XrGeometryFixture, "XR pointing intersects the current authored draw with bounded atomic output",
                     "[unit][editor][xr]") {
        using namespace Horo;
        std::array<Editor::EditorXrGeometryHit, 1> hits;
        const Math::Ray pointed{{2.1F, 3.1F, 4.0F}, {0, 0, -1}, 0, 8};
        const auto result = prepared->QueryPointing(pointed, hits);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value() == 1);
        CHECK(hits[0].object.value == objects.child.value);
        CHECK(NearlyEqual(hits[0].distanceMeters, 3.5F));
        CHECK(prepared->QueryPointing(pointed, {}).HasError());
        auto shortRay = pointed;
        shortRay.maximumDistance = 3;
        const auto missed = prepared->QueryPointing(shortRay, hits);
        REQUIRE(missed.HasValue());
        CHECK(missed.Value() == 0);
        CHECK(hits[0].object.value == objects.child.value);
        prepared->Revoke();
        CHECK(prepared->QueryPointing(pointed, hits).HasError());
    }

    TEST_CASE("XR pointing orders triangle hits and rejects bounds-only intersections", "[unit][editor][xr][pointing]") {
        using namespace Horo;
        const std::array vertices{Render::MeshVertex{.position = {0, 0, 0}}, Render::MeshVertex{.position = {1, 0, 0}},
                                  Render::MeshVertex{.position = {0, 1, 0}}};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        const Math::Aabb bounds{{0, 0, 0}, {1, 1, 0}};
        const Render::RenderMeshSourceHandle handle{{1}, 1};
        const std::array meshes{Render::RenderMeshResourceView{handle, vertices, indices, bounds}};
        const auto near = Math::Transform{.translation = {0, 0, 2}, .scale = {1, 1, 2}}.ToMatrix();
        const std::array instances{Render::RenderStaticMeshInstance{handle, Math::Mat4::Identity(), bounds},
                                   Render::RenderStaticMeshInstance{handle, near, bounds}};
        const std::array objects{Render::RenderMotionObjectId{1}, Render::RenderMotionObjectId{2}};
        Math::Ray ray{{0.2F, 0.2F, 4}, {0, 0, -1}, 0, 8};
        std::array<Editor::EditorXrGeometryHit, 2> hits;
        using Editor::XrSceneDetail::QueryPointingDraws;
        const auto result = QueryPointingDraws(ray, instances, objects, meshes, hits);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value() == 2);
        CHECK(hits[0].object.value == 2);
        CHECK(hits[1].object.value == 1);
        CHECK(NearlyEqual(hits[0].distanceMeters, 2));
        CHECK(NearlyEqual(hits[1].distanceMeters, 4));
        hits.front() = {{}, -1};
        CHECK(QueryPointingDraws(ray, instances, objects, meshes, std::span{hits}.first(1)).HasError());
        CHECK(hits.front().distanceMeters == -1);
        ray.origin = {0.8F, 0.8F, 4};
        const auto missed = QueryPointingDraws(ray, instances, objects, meshes, hits);
        REQUIRE(missed.HasValue());
        CHECK(missed.Value() == 0);
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

}  // namespace
