#include "Horo/Runtime/Scene/PrimitiveCatalog.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/document/SceneDocument.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace {
    [[nodiscard]] Horo::Prefab::PrefabAssetReference PrefabAsset(const std::uint8_t suffix = 1) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = suffix;
        return Horo::Prefab::PrefabAssetReference::Create(Horo::Assets::AssetId::FromBytes(bytes)).Value();
    }

    [[nodiscard]] Horo::Runtime::NavigationSurfaceComponent NavigationSurface(const std::uint64_t id = 1) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 7;
        return {
            .id = Horo::Navigation::SurfaceId::Create(id).Value(),
            .definition = Horo::Assets::AssetId::FromBytes(bytes),
            .profiles = {Horo::Navigation::NavigationAgentProfileId::Create(3).Value()},
        };
    }

    [[nodiscard]] Horo::Runtime::NavigationRegionComponent NavigationRegion(const std::uint64_t id = 1, const std::uint64_t surface = 1) {
        return {
            .id = Horo::Navigation::NavigationRegionId::Create(id).Value(),
            .surface = Horo::Navigation::SurfaceId::Create(surface).Value(),
        };
    }

    [[nodiscard]] Horo::Runtime::NavigationModifierComponent NavigationModifier(const std::uint64_t id = 1,
                                                                                const std::uint64_t surface = 1) {
        return {
            .id = Horo::Navigation::NavigationModifierId::Create(id).Value(),
            .surface = Horo::Navigation::SurfaceId::Create(surface).Value(),
        };
    }

    [[nodiscard]] Horo::Runtime::NavigationLinkComponent NavigationLink(const std::uint64_t id = 1, const std::uint64_t surface = 1) {
        return {
            .id = Horo::Navigation::NavigationLinkId::Create(id).Value(),
            .start = {.surface = Horo::Navigation::SurfaceId::Create(surface).Value()},
            .end = {.surface = Horo::Navigation::SurfaceId::Create(surface).Value(), .localPosition = {3.0F, 0.0F, 0.0F}},
            .profiles = {Horo::Navigation::NavigationAgentProfileId::Create(3).Value()},
        };
    }

    [[nodiscard]] std::unique_ptr<Horo::Runtime::RuntimeScene> MakeRuntimeScene(const Horo::Editor::SceneDocument &document) {
        auto definition = Horo::Editor::ConvertSceneDocumentToRuntime(document.Snapshot(), Horo::Runtime::SceneDefinitionId{1});
        REQUIRE((definition.HasValue()));
        auto scene = Horo::Runtime::RuntimeScene::Create(definition.Value(), Horo::Runtime::SceneRuntimeId{1});
        REQUIRE((scene.HasValue()));
        return std::move(scene).Value();
    }

    [[nodiscard]] bool NearlyEqual(const float lhs, const float rhs) noexcept {
        return std::fabs(lhs - rhs) < 0.0001F;
    }

    [[nodiscard]] Horo::Prefab::PrefabLimitProfile ScenePrefabLimits(Horo::Prefab::PrefabProjectPolicy policy = {}) {
        return Horo::Prefab::PrefabLimitProfile::Create(std::move(policy)).Value();
    }

    [[nodiscard]] Horo::Application::HoroVersion ScenePrefabVersion() {
        return Horo::Application::ParseHoroVersion("1.2.3").Value();
    }

    [[nodiscard]] Horo::Prefab::PrefabSourceRevision ScenePrefabRevision(const std::uint8_t suffix = 1) {
        Horo::Sha256Digest digest{};
        digest.bytes.back() = suffix;
        return {ScenePrefabVersion(), digest};
    }

    [[nodiscard]] Horo::Prefab::PrefabDocument ScenePrefabDocument(const Horo::Assets::AssetId asset,
                                                                   std::vector<Horo::Prefab::PrefabObjectNode> objects) {
        return Horo::Prefab::PrefabDocument::Create({.projectVersion = ScenePrefabVersion(),
                                                     .assetId = asset,
                                                     .objects = std::move(objects)},
                                                    ScenePrefabLimits())
            .Value();
    }

    [[nodiscard]] Horo::Assets::AssetRecord ScenePrefabRecord(const Horo::Assets::AssetId asset, const std::string &name) {
        return {asset, Horo::Assets::AssetTypeId::Parse("core.prefab").Value(),
                Horo::ProjectPath::Parse("assets/prefabs/" + name + ".prefab").Value(),
                Horo::ProjectPath::Parse("assets/prefabs/" + name + ".prefab.horo").Value()};
    }

    [[nodiscard]] Horo::Prefab::PrefabSourceResolverSnapshot ScenePrefabResolver(const Horo::Assets::AssetId asset,
                                                                                 std::vector<Horo::Prefab::PrefabObjectNode> objects) {
        Horo::Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({ScenePrefabRecord(asset, "scene-test")}).status == Horo::Assets::AssetRegistryBuildStatus::Complete);
        return Horo::Prefab::BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                               {{ScenePrefabDocument(asset, std::move(objects)), ScenePrefabRevision()}},
                                                               ScenePrefabLimits())
            .Value();
    }

    TEST_CASE("Viewport Scene State Advances Its Mesh Resource Generation Only For Resource Changes", "[unit][editor]") {
        using namespace Horo::Editor;

        EditorViewportSceneState state;
        REQUIRE((state.MeshResourceGeneration() == 0));

        state.Replace({});
        REQUIRE((state.MeshResourceGeneration() == 0));

        EditorViewportSceneSnapshot cameraOnlySnapshot;
        cameraOnlySnapshot.camera.position = {1.0F, 2.0F, 3.0F};
        state.Replace(std::move(cameraOnlySnapshot));
        REQUIRE((state.MeshResourceGeneration() == 0));

        EditorViewportSceneSnapshot meshSnapshot;
        meshSnapshot.meshResources.push_back({.handle = Horo::Render::RenderMeshSourceHandle{Horo::Render::MeshResourceId{1}, 1},
                                              .vertices = {},
                                              .indices = {},
                                              .localBounds = {}});
        state.Replace(meshSnapshot);
        REQUIRE((state.MeshResourceGeneration() == 1));

        meshSnapshot.camera.position = {4.0F, 5.0F, 6.0F};
        state.Replace(meshSnapshot);
        REQUIRE((state.MeshResourceGeneration() == 1));

        meshSnapshot.meshResources.front().handle.generation = 2;
        state.Replace(std::move(meshSnapshot));
        REQUIRE((state.MeshResourceGeneration() == 2));

        state.Clear();
        REQUIRE((state.MeshResourceGeneration() == 3));

        state.Clear();
        REQUIRE((state.MeshResourceGeneration() == 3));
    }

    TEST_CASE("Disabled core components remain authored but are omitted from runtime conversion", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;

        SceneDocumentSnapshot document{
            .revision = DocumentRevision{1},
            .state = DocumentStateId{1},
            .objects =
                {
                    SceneObjectSnapshot{
                        .id = SceneObjectId{1},
                        .name = "Disabled Camera",
                        .components = {.camera = Runtime::CameraComponent{.enabled = false}},
                    },
                    SceneObjectSnapshot{
                        .id = SceneObjectId{2},
                        .name = "Disabled Navigation",
                        .components = {.navigationSurface = NavigationSurface(),
                                       .navigationModifier = NavigationModifier(),
                                       .navigationLink = NavigationLink()},
                    },
                },
        };
        document.objects[1].components.navigationSurface->enabled = false;
        document.objects[1].components.navigationModifier->enabled = false;
        document.objects[1].components.navigationLink->enabled = false;
        const auto converted = ConvertSceneDocumentToRuntime(document, Runtime::SceneDefinitionId{1});
        REQUIRE(converted.HasValue());
        REQUIRE(converted.Value().Entities().size() == 2);
        REQUIRE_FALSE(converted.Value().Entities().front().components.camera.has_value());
        REQUIRE(document.objects.front().components.camera.has_value());
        REQUIRE_FALSE(converted.Value().Entities()[1].components.navigationSurface.has_value());
        REQUIRE_FALSE(converted.Value().Entities()[1].components.navigationModifier.has_value());
        REQUIRE_FALSE(converted.Value().Entities()[1].components.navigationLink.has_value());
        REQUIRE(document.objects[1].components.navigationLink.has_value());
    }

    TEST_CASE("Scene navigation commands preserve committed generations through undo redo and runtime conversion",
              "[unit][editor][navigation]") {
        using namespace Horo;
        using namespace Horo::Editor;

        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto surfaceObject = commands.Execute(CreateSceneObjectCommand{.name = "Surface"});
        const auto regionObject = commands.Execute(CreateSceneObjectCommand{.name = "Region"});
        REQUIRE(surfaceObject.HasValue());
        REQUIRE(regionObject.HasValue());
        REQUIRE(commands.Execute(SetSceneNavigationSurfaceCommand{surfaceObject.Value().object, NavigationSurface()}).HasValue());
        REQUIRE(commands.Execute(SetSceneNavigationRegionCommand{regionObject.Value().object, NavigationRegion()}).HasValue());
        REQUIRE(commands.Execute(SetSceneNavigationModifierCommand{regionObject.Value().object, NavigationModifier()}).HasValue());
        REQUIRE(commands.Execute(SetSceneNavigationLinkCommand{regionObject.Value().object, NavigationLink()}).HasValue());

        auto changedSurface = NavigationSurface();
        changedSurface.generation = 2;
        REQUIRE(commands.Execute(SetSceneNavigationSurfaceCommand{surfaceObject.Value().object, changedSurface}).HasValue());
        REQUIRE(document.Objects()[0].components.navigationSurface->generation == 2);
        REQUIRE(commands.Undo().HasValue());
        REQUIRE(document.Objects()[0].components.navigationSurface->generation == 1);
        REQUIRE(commands.Redo().HasValue());
        REQUIRE(document.Objects()[0].components.navigationSurface->generation == 2);

        const auto runtime = ConvertSceneDocumentToRuntime(document.Snapshot(), Runtime::SceneDefinitionId{9});
        REQUIRE(runtime.HasValue());
        REQUIRE(runtime.Value().Revision().value == document.State().value);
        REQUIRE(runtime.Value().Entities()[0].components.navigationSurface->generation == 2);
        REQUIRE(runtime.Value().Entities()[1].components.navigationRegion->surface == Navigation::SurfaceId::Create(1).Value());
        REQUIRE(runtime.Value().Entities()[1].components.navigationModifier->surface == Navigation::SurfaceId::Create(1).Value());
        REQUIRE(runtime.Value().Entities()[1].components.navigationLink->direction == Runtime::NavigationLinkDirection::StartToEnd);

        const DocumentRevision revision = document.Revision();
        auto malformedModifier = NavigationModifier();
        malformedModifier.operation = Runtime::NavigationModifierOperation::OverrideArea;
        REQUIRE(commands.Execute(SetSceneNavigationModifierCommand{regionObject.Value().object, malformedModifier}).HasError());
        auto incompatibleLink = NavigationLink();
        incompatibleLink.profiles = {Navigation::NavigationAgentProfileId::Create(99).Value()};
        REQUIRE(commands.Execute(SetSceneNavigationLinkCommand{regionObject.Value().object, incompatibleLink}).HasError());
        REQUIRE(document.Revision() == revision);
        REQUIRE(document.Objects()[0].components.navigationSurface->generation == 2);
        REQUIRE(document.Objects()[1].components.navigationModifier == NavigationModifier());
        REQUIRE(document.Objects()[1].components.navigationLink == NavigationLink());
        REQUIRE(commands.Execute(SetSceneNavigationSurfaceCommand{surfaceObject.Value().object, std::nullopt}).HasError());
        REQUIRE(commands.Execute(SetSceneNavigationRegionCommand{regionObject.Value().object, NavigationRegion(1, 99)}).HasError());
    }

    TEST_CASE("Scene object duplication regenerates navigation component identities and retargets local regions",
              "[unit][editor][navigation]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        SceneObjectComponentSet components;
        components.navigationSurface = NavigationSurface(8);
        components.navigationRegion = NavigationRegion(11, 8);
        components.navigationModifier = NavigationModifier(17, 8);
        components.navigationLink = NavigationLink(23, 8);
        const auto source = commands.Execute(CreateSceneObjectCommand{.name = "Source", .components = components});
        REQUIRE(source.HasValue());
        const auto duplicate = commands.Execute(DuplicateSceneObjectCommand{source.Value().object, "Duplicate"});
        REQUIRE(duplicate.HasValue());
        const auto &duplicated = document.Objects().back().components;
        REQUIRE(duplicated.navigationSurface->id != components.navigationSurface->id);
        REQUIRE(duplicated.navigationRegion->id != components.navigationRegion->id);
        REQUIRE(duplicated.navigationModifier->id != components.navigationModifier->id);
        REQUIRE(duplicated.navigationLink->id != components.navigationLink->id);
        REQUIRE(duplicated.navigationSurface->id.Value() == 9);
        REQUIRE(duplicated.navigationRegion->id.Value() == 12);
        REQUIRE(duplicated.navigationRegion->surface == duplicated.navigationSurface->id);
        REQUIRE(duplicated.navigationModifier->surface == duplicated.navigationSurface->id);
        REQUIRE(duplicated.navigationLink->start.surface == duplicated.navigationSurface->id);
        REQUIRE(duplicated.navigationLink->end.surface == duplicated.navigationSurface->id);
    }

    TEST_CASE("Catalog Owns Stable Core Primitive Ids", "[unit][editor]") {
        using namespace Horo::Runtime;
        const PrimitiveDescriptor *box = PrimitiveCatalog::Find("primitive.mesh.box");
        REQUIRE((box != nullptr));
        REQUIRE((box->id == PrimitiveId{"primitive.mesh.box"}));
        REQUIRE((box->id.IsValid()));
        REQUIRE((box->category == PrimitiveCategory::Mesh));
        REQUIRE((box->creationGroup == PrimitiveCreationGroup::Objects3D));
        REQUIRE((box->defaultObjectName == "Box"));
        REQUIRE((!box->iconToken.empty()));
        REQUIRE((box->displayName == "Cube / Box"));
        REQUIRE((box->meshType == PrimitiveMeshType::Box));
        REQUIRE((box->defaultCollider == ColliderShapeType::Box));
        REQUIRE((box->isRenderable && box->isPhysicsSolidByDefault));
        REQUIRE((PrimitiveCatalog::Find(PrimitiveMeshType::Box) == box));
        REQUIRE((PrimitiveCatalog::All().size() == 18));
        REQUIRE((PrimitiveCatalog::Find("primitive.mesh.missing") == nullptr));
    }

    TEST_CASE("Catalog Creation Use Case Creates Every Core Hierarchy Primitive", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        using namespace Horo::Runtime;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        CreateSceneObjectUseCase create{document, commands};

        std::size_t creatableCount = 0;
        for (const PrimitiveDescriptor &descriptor : PrimitiveCatalog::All()) {
            if (descriptor.creationGroup == PrimitiveCreationGroup::NotCreatable) {
                continue;
            }
            const auto created = create.Execute(PrimitiveCreationRequest{descriptor.id, std::nullopt});
            REQUIRE((created.HasValue()));
            ++creatableCount;
            const SceneObjectSnapshot &object = document.Objects().back();
            REQUIRE((object.name == descriptor.defaultObjectName));
            REQUIRE((object.primitiveMesh.has_value() == descriptor.meshType.has_value()));
            if (descriptor.sceneObjectType == SceneObjectPrimitiveType::Camera)
                REQUIRE((object.components.camera.has_value()));
            if (descriptor.sceneObjectType == SceneObjectPrimitiveType::DirectionalLight)
                REQUIRE((object.components.light->kind == LightKind::Directional));
            if (descriptor.sceneObjectType == SceneObjectPrimitiveType::PointLight)
                REQUIRE((object.components.light->kind == LightKind::Point));
            if (descriptor.sceneObjectType == SceneObjectPrimitiveType::SpotLight)
                REQUIRE((object.components.light->kind == LightKind::Spot));
            if (descriptor.sceneObjectType == SceneObjectPrimitiveType::TriggerVolume)
                REQUIRE((object.components.triggerVolume.has_value()));
            if (descriptor.sceneObjectType == SceneObjectPrimitiveType::AudioSource)
                REQUIRE((object.components.audioSource.has_value()));
        }
        REQUIRE((creatableCount == 14));
        REQUIRE((document.Objects().size() == 14));

        const DocumentRevision revision = document.Revision();
        REQUIRE((create.Execute(PrimitiveCreationRequest{PrimitiveId{"primitive.collider.box"}, std::nullopt}).HasError()));
        REQUIRE((create.Execute(PrimitiveCreationRequest{PrimitiveId{"primitive.missing"}, std::nullopt}).HasError()));
        REQUIRE((document.Revision() == revision));
    }

    TEST_CASE("Creation Names Are Unique Per Sibling And Typed Components Survive History", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        using namespace Horo::Runtime;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        CreateSceneObjectUseCase create{document, commands};

        const auto root = create.Execute({PrimitiveId{"primitive.object.empty"}, std::nullopt});
        REQUIRE((root.HasValue()));
        const auto first = create.Execute({PrimitiveId{"primitive.object.camera"}, root.Value().object});
        const auto second = create.Execute({PrimitiveId{"primitive.object.camera"}, root.Value().object});
        const auto otherRoot = create.Execute({PrimitiveId{"primitive.object.camera"}, std::nullopt});
        REQUIRE((first.HasValue() && second.HasValue() && otherRoot.HasValue()));
        REQUIRE((document.Objects()[1].name == "Camera"));
        REQUIRE((document.Objects()[2].name == "Camera 2"));
        REQUIRE((document.Objects()[3].name == "Camera"));
        REQUIRE((document.Objects()[2].components.camera.has_value()));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((document.Objects().back().components.camera.has_value()));
    }

    TEST_CASE("Directional Light Kind Survives Duplicate Undo And Redo", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        using namespace Horo::Runtime;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        CreateSceneObjectUseCase create{document, commands};

        const auto created = create.Execute({PrimitiveId{"primitive.object.light_directional"}, std::nullopt});
        REQUIRE((created.HasValue()));
        REQUIRE((document.Objects().front().components.light->kind == LightKind::Directional));

        const auto duplicated = commands.Execute(DuplicateSceneObjectCommand{created.Value().object, "Sun Copy"});
        REQUIRE((duplicated.HasValue()));
        REQUIRE((document.Objects().back().components.light->kind == LightKind::Directional));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((document.Objects().size() == 1));
        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((document.Objects().back().components.light->kind == LightKind::Directional));
    }

    TEST_CASE("Editor Visibility And Lock Resolve Through Parents And Survive History", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto parent = commands.Execute(CreateSceneObjectCommand{.name = "Parent"});
        REQUIRE((parent.HasValue()));
        const auto child = commands.Execute(CreateSceneObjectCommand{.name = "Child", .parent = parent.Value().object});
        REQUIRE((child.HasValue()));

        REQUIRE(
            (commands
                 .Execute(SetSceneObjectEditorStateCommand{parent.Value().object, SceneObjectEditorState{.visible = false, .locked = true}})
                 .HasValue()));
        const auto inherited = ResolveSceneObjectEditorState(document.Objects(), child.Value().object);
        REQUIRE((inherited.has_value()));
        REQUIRE_FALSE((inherited->effectivelyVisible));
        REQUIRE((inherited->effectivelyLocked));
        REQUIRE((inherited->hiddenByParent));
        REQUIRE((inherited->lockedByParent));
        REQUIRE((inherited->local == SceneObjectEditorState{}));

        REQUIRE((commands.Execute(RenameSceneObjectCommand{child.Value().object, "Blocked"}).HasError()));
        REQUIRE((commands.Execute(SetSceneObjectTransformCommand{child.Value().object, Math::Transform{}}).HasError()));
        REQUIRE((commands.Execute(DeleteSceneObjectCommand{parent.Value().object}).HasError()));

        REQUIRE((commands.Undo().HasValue()));
        const auto restored = ResolveSceneObjectEditorState(document.Objects(), child.Value().object);
        REQUIRE((restored.has_value() && restored->effectivelyVisible && !restored->effectivelyLocked));
        REQUIRE((commands.Redo().HasValue()));
        const auto redone = ResolveSceneObjectEditorState(document.Objects(), parent.Value().object);
        REQUIRE((redone.has_value() && !redone->local.visible && redone->local.locked));
    }

    TEST_CASE("Hidden Unlocked Objects Duplicate Their Local Editor State", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Hidden"});
        REQUIRE((created.HasValue()));
        REQUIRE((commands
                     .Execute(SetSceneObjectEditorStateCommand{created.Value().object,
                                                               SceneObjectEditorState{.visible = false, .locked = false}})
                     .HasValue()));
        const auto duplicated = commands.Execute(DuplicateSceneObjectCommand{created.Value().object, "Hidden Copy"});
        REQUIRE((duplicated.HasValue()));
        REQUIRE_FALSE((document.Objects().back().editorState.visible));
        REQUIRE_FALSE((document.Objects().back().editorState.locked));
    }

    TEST_CASE("Camera Component Editing Is Validated And Survives History", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        using namespace Horo::Runtime;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        CreateSceneObjectUseCase create{document, commands};

        const auto created = create.Execute({PrimitiveId{"primitive.object.camera"}, std::nullopt});
        REQUIRE((created.HasValue()));
        const CameraComponent original = *document.Objects().front().components.camera;
        const DocumentRevision initialRevision = document.Revision();

        CameraComponent edited = original;
        edited.projection = CameraProjection::Orthographic;
        edited.orthographicHeight = 18.0F;
        edited.nearPlane = 0.25F;
        edited.farPlane = 2400.0F;
        const auto committed = commands.Execute(SetSceneObjectCameraCommand{created.Value().object, edited});
        REQUIRE((committed.HasValue()));
        REQUIRE((committed.Value().committed));
        REQUIRE((committed.Value().kind == DocumentChangeKind::ComponentChanged));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.camera == edited));

        const auto noOp = commands.Execute(SetSceneObjectCameraCommand{created.Value().object, edited});
        REQUIRE((noOp.HasValue()));
        REQUIRE((!noOp.Value().committed));
        REQUIRE((document.Revision().value == initialRevision.value + 1));

        CameraComponent invalid = edited;
        invalid.farPlane = invalid.nearPlane;
        const auto rejected = commands.Execute(SetSceneObjectCameraCommand{created.Value().object, invalid});
        REQUIRE((rejected.HasError()));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.camera == edited));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((*document.Objects().front().components.camera == original));
        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((*document.Objects().front().components.camera == edited));
    }

    TEST_CASE("Light Component Editing Is Validated And Survives History", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        using namespace Horo::Runtime;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        CreateSceneObjectUseCase create{document, commands};

        const auto created = create.Execute({PrimitiveId{"primitive.object.light_point"}, std::nullopt});
        REQUIRE((created.HasValue()));
        const LightComponent original = *document.Objects().front().components.light;
        const DocumentRevision initialRevision = document.Revision();

        LightComponent edited = original;
        edited.kind = LightKind::Spot;
        edited.color = {0.25F, 0.5F, 0.75F};
        edited.intensity = 3.5F;
        edited.range = 42.0F;
        edited.innerConeRadians = 0.3F;
        edited.outerConeRadians = 0.8F;
        const auto committed = commands.Execute(SetSceneObjectLightCommand{created.Value().object, edited});
        REQUIRE((committed.HasValue()));
        REQUIRE((committed.Value().committed));
        REQUIRE((committed.Value().kind == DocumentChangeKind::ComponentChanged));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.light == edited));

        const auto noOp = commands.Execute(SetSceneObjectLightCommand{created.Value().object, edited});
        REQUIRE((noOp.HasValue()));
        REQUIRE((!noOp.Value().committed));
        REQUIRE((document.Revision().value == initialRevision.value + 1));

        LightComponent invalid = edited;
        invalid.outerConeRadians = invalid.innerConeRadians - 0.1F;
        const auto rejected = commands.Execute(SetSceneObjectLightCommand{created.Value().object, invalid});
        REQUIRE((rejected.HasError()));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.light == edited));

        invalid = edited;
        invalid.color.x = -0.1F;
        REQUIRE((commands.Execute(SetSceneObjectLightCommand{created.Value().object, invalid}).HasError()));
        invalid = edited;
        invalid.outerConeRadians = Math::Pi + 0.1F;
        REQUIRE((commands.Execute(SetSceneObjectLightCommand{created.Value().object, invalid}).HasError()));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.light == edited));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((*document.Objects().front().components.light == original));
        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((*document.Objects().front().components.light == edited));
    }

    TEST_CASE("Batch Transform Commits Atomically Into One History Entry", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};

        const auto first = commands.Execute(CreateSceneObjectCommand{.name = "First"});
        const auto second = commands.Execute(CreateSceneObjectCommand{.name = "Second"});
        REQUIRE((first.HasValue() && second.HasValue()));

        const DocumentRevision beforeRevision = document.Revision();
        const SetSceneObjectTransformsCommand batch{{
            SceneObjectTransformUpdate{
                .object = first.Value().object,
                .localTransform = Math::Transform{.translation = {2.0F, 0.0F, 0.0F}},
            },
            SceneObjectTransformUpdate{
                .object = second.Value().object,
                .localTransform = Math::Transform{.translation = {0.0F, 3.0F, 0.0F}},
            },
        }};
        const Result<SceneCommandResult> committed = commands.Execute(batch);
        REQUIRE((committed.HasValue()));
        REQUIRE((committed.Value().committed));
        REQUIRE((committed.Value().affectedObjects == std::vector{first.Value().object, second.Value().object}));
        REQUIRE((document.Revision().value == beforeRevision.value + 1));
        REQUIRE((document.Objects()[0].localTransform.translation.x == 2.0F));
        REQUIRE((document.Objects()[1].localTransform.translation.y == 3.0F));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((document.Objects()[0].localTransform.translation == Math::Vec3{}));
        REQUIRE((document.Objects()[1].localTransform.translation == Math::Vec3{}));

        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((document.Objects()[0].localTransform.translation.x == 2.0F));
        REQUIRE((document.Objects()[1].localTransform.translation.y == 3.0F));
        REQUIRE((!history.CanRedo()));
    }

    TEST_CASE("Failed Commands Do Not Mutate Or Advance Revision", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        CreateSceneObjectCommand invalid{.name = "Invalid"};
        invalid.localTransform.translation.x = std::numeric_limits<float>::quiet_NaN();

        const auto result = commands.Execute(invalid);
        REQUIRE((result.HasError()));
        REQUIRE((document.Revision() == DocumentRevision{}));
        REQUIRE((document.Snapshot().objects.empty()));
        REQUIRE((!document.IsDirty()));
    }

    TEST_CASE("Euler Conversion Preserves The Composed Rotation", "[unit][editor]") {
        using namespace Horo::Math;
        const Vec3 authored{0.31F, -0.47F, 0.22F};
        const Quaternion rotation = Quaternion::FromEulerRadians(authored);
        const Vec3 recovered = rotation.ToEulerRadians();
        REQUIRE((NearlyEqual(recovered.x, authored.x)));
        REQUIRE((NearlyEqual(recovered.y, authored.y)));
        REQUIRE((NearlyEqual(recovered.z, authored.z)));

        const Mat4 originalMatrix = Transform{.rotation = rotation}.ToMatrix();
        const Mat4 recoveredMatrix = Transform{.rotation = Quaternion::FromEulerRadians(recovered)}.ToMatrix();
        for (std::size_t index = 0; index < originalMatrix.values.size(); ++index) {
            REQUIRE((NearlyEqual(originalMatrix.values[index], recoveredMatrix.values[index])));
        }
    }

    TEST_CASE("Affine Inverse Restores Transformed Points", "[unit][editor]") {
        using namespace Horo::Math;
        const Mat4 matrix =
            Transform{
                .translation = {2.0F, -3.0F, 4.0F},
                .rotation = Quaternion::FromEulerRadians({0.2F, -0.4F, 0.3F}),
                .scale = {2.0F, 1.5F, 0.5F},
            }
                .ToMatrix();
        const Horo::Result<Mat4> inverse = TryInverseAffine(matrix);
        REQUIRE((inverse.HasValue()));
        const Vec3 point{0.7F, -1.2F, 2.4F};
        const Vec3 restored = TransformAffinePoint(inverse.Value(), TransformAffinePoint(matrix, point));
        REQUIRE((NearlyEqual(restored.x, point.x)));
        REQUIRE((NearlyEqual(restored.y, point.y)));
        REQUIRE((NearlyEqual(restored.z, point.z)));
        REQUIRE((TryInverseAffine(Transform{.scale = {0.0F, 1.0F, 1.0F}}.ToMatrix()).HasError()));
    }

    TEST_CASE("Commands Create Stable Objects And Track Saved Revision", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{
            .name = "Box",
            .primitiveMesh = PrimitiveMeshDescriptor{},
        });
        REQUIRE((created.HasValue()));
        REQUIRE((created.Value().object.IsValid()));
        REQUIRE((document.Revision().value == 1));
        REQUIRE((document.IsDirty()));
        REQUIRE((document.MarkSaved(document.Revision(), document.State()).HasValue()));
        REQUIRE((!document.IsDirty()));

        const auto renamed = commands.Execute(RenameSceneObjectCommand{created.Value().object, "Hero Box"});
        REQUIRE((renamed.HasValue()));
        REQUIRE((document.Revision().value == 2));
        REQUIRE((document.IsDirty()));
        REQUIRE((document.Snapshot().objects.front().name == "Hero Box"));
    }

    TEST_CASE("Unchanged Transform Does Not Create A History Entry", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Box"});
        REQUIRE((created.HasValue()));
        history.Clear();
        const SceneDocumentSnapshot before = document.Snapshot();

        const auto unchanged =
            commands.Execute(SetSceneObjectTransformCommand{created.Value().object, before.objects.front().localTransform});
        REQUIRE((unchanged.HasValue()));
        REQUIRE((!unchanged.Value().committed));
        REQUIRE((document.Revision() == before.revision));
        REQUIRE((document.State() == before.state));
        REQUIRE((!history.CanUndo()));
    }

    TEST_CASE("Extraction Resolves Hierarchy Into World Matrices", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        Runtime::PrimitiveMeshCache meshCache;
        const auto parent = commands.Execute(CreateSceneObjectCommand{
            .name = "Parent",
            .localTransform = Math::Transform{.translation = {2.0F, 0.0F, 0.0F}},
        });
        REQUIRE((parent.HasValue()));
        const auto child = commands.Execute(CreateSceneObjectCommand{
            .name = "Box",
            .parent = parent.Value().object,
            .localTransform = Math::Transform{.translation = {0.0F, 3.0F, 0.0F}},
            .primitiveMesh = PrimitiveMeshDescriptor{},
        });
        REQUIRE((child.HasValue()));

        const auto runtimeScene = MakeRuntimeScene(document);
        const auto extracted = ExtractEditorViewportScene(runtimeScene->View(), document.Revision(), EditorViewportCamera{}, meshCache);
        REQUIRE((extracted.HasValue()));
        REQUIRE((extracted.Value().documentRevision == document.Revision()));
        REQUIRE((extracted.Value().instances.size() == 1));
        REQUIRE((extracted.Value().instanceObjects == std::vector{child.Value().object}));
        const Math::Vec3 worldOrigin = Math::TransformPoint(extracted.Value().instances.front().localToWorld, {});
        REQUIRE((NearlyEqual(worldOrigin.x, 2.0F)));
        REQUIRE((NearlyEqual(worldOrigin.y, 3.0F)));
        REQUIRE((NearlyEqual(worldOrigin.z, 0.0F)));
        const auto resolved = ResolveSceneObjectWorldTransforms(runtimeScene->View(), child.Value().object);
        REQUIRE((resolved.HasValue()));
        const Math::Vec3 resolvedOrigin = Math::TransformPoint(resolved.Value().localToWorld, {});
        const Math::Vec3 resolvedParentOrigin = Math::TransformPoint(resolved.Value().parentToWorld, {});
        REQUIRE((NearlyEqual(resolvedOrigin.x, 2.0F) && NearlyEqual(resolvedOrigin.y, 3.0F)));
        REQUIRE((NearlyEqual(resolvedParentOrigin.x, 2.0F) && NearlyEqual(resolvedParentOrigin.y, 0.0F)));

        EditorViewportSceneSnapshot previewScene = extracted.Value();
        const SceneObjectTransformPreview preview{
            .object = parent.Value().object,
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
                .object = child.Value().object,
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

    TEST_CASE("Extraction Produces World Space Lights And Applies Transform Preview", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        Runtime::PrimitiveMeshCache meshCache;

        const auto parent = commands.Execute(CreateSceneObjectCommand{
            .name = "Light Rig",
            .localTransform = Math::Transform{.translation = {2.0F, 0.0F, 0.0F}},
        });
        REQUIRE((parent.HasValue()));
        SceneObjectComponentSet components;
        components.light = Runtime::LightComponent{
            .kind = Runtime::LightKind::Spot,
            .color = {0.5F, 0.75F, 1.0F},
            .intensity = 3.0F,
            .range = 24.0F,
            .innerConeRadians = 0.25F,
            .outerConeRadians = 0.75F,
        };
        const auto lightObject = commands.Execute(CreateSceneObjectCommand{
            .name = "Spot",
            .parent = parent.Value().object,
            .localTransform =
                Math::Transform{
                    .translation = {0.0F, 3.0F, 0.0F},
                    .rotation = Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, Math::Pi * 0.5F),
                },
            .components = components,
        });
        REQUIRE((lightObject.HasValue()));

        const auto runtimeScene = MakeRuntimeScene(document);
        const auto extracted = ExtractEditorViewportScene(runtimeScene->View(), document.Revision(), EditorViewportCamera{}, meshCache);
        REQUIRE((extracted.HasValue()));
        REQUIRE((extracted.Value().lights.size() == 1));
        REQUIRE((extracted.Value().lightObjects == std::vector{lightObject.Value().object}));
        const Render::RenderLight &light = extracted.Value().lights.front();
        REQUIRE((light.kind == Render::RenderLightKind::Spot));
        REQUIRE((Math::NearlyEqual(light.position, Math::Vec3{2.0F, 3.0F, 0.0F})));
        REQUIRE((NearlyEqual(light.innerConeCosine, std::cos(0.25F))));
        REQUIRE((NearlyEqual(light.outerConeCosine, std::cos(0.75F))));
        REQUIRE((light.IsValid()));

        EditorViewportSceneSnapshot preview = extracted.Value();
        const std::array previews{
            SceneObjectTransformPreview{
                .object = parent.Value().object,
                .localTransform = Math::Transform{.translation = {5.0F, 0.0F, 0.0F}},
            },
            SceneObjectTransformPreview{
                .object = lightObject.Value().object,
                .localTransform = Math::Transform{.translation = {0.0F, 4.0F, 0.0F}},
            },
        };
        REQUIRE((ApplyEditorViewportTransformPreview(runtimeScene->View(), previews, preview).HasValue()));
        REQUIRE((Math::NearlyEqual(preview.lights.front().position, Math::Vec3{5.0F, 4.0F, 0.0F})));
        REQUIRE((Math::NearlyEqual(preview.lights.front().direction, Math::Vec3{0.0F, 0.0F, -1.0F})));

        Runtime::LightComponent previewComponent = *components.light;
        previewComponent.intensity = 9.0F;
        previewComponent.range = 40.0F;
        previewComponent.innerConeRadians = 0.4F;
        const SceneObjectLightPreview lightPreview{lightObject.Value().object, previewComponent};
        REQUIRE((ApplyEditorViewportLightPreview(runtimeScene->View(), &lightPreview, preview).HasValue()));
        REQUIRE((NearlyEqual(preview.lights.front().intensity, 9.0F)));
        REQUIRE((NearlyEqual(preview.lights.front().range, 40.0F)));
        REQUIRE((NearlyEqual(preview.lights.front().innerConeCosine, std::cos(0.4F))));
        REQUIRE((ApplyEditorViewportLightPreview(runtimeScene->View(), nullptr, preview).HasValue()));
        REQUIRE((NearlyEqual(preview.lights.front().intensity, components.light->intensity)));
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
