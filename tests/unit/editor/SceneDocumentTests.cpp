#include "Horo/Runtime/Scene/PrimitiveCatalog.h"
#include "SceneDocumentTestSupport.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/document/SceneDocument.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
    using namespace Horo::Editor::SceneDocumentTestSupport;

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
                                       .navigationLink = NavigationLink(),
                                       .navigationAgent = NavigationAgent()},
                    },
                },
        };
        document.objects[1].components.navigationSurface->enabled = false;
        document.objects[1].components.navigationModifier->enabled = false;
        document.objects[1].components.navigationLink->enabled = false;
        document.objects[1].components.navigationAgent->enabled = false;
        const auto converted = ConvertSceneDocumentToRuntime(document, Runtime::SceneDefinitionId{1});
        REQUIRE(converted.HasValue());
        REQUIRE(converted.Value().Entities().size() == 2);
        REQUIRE_FALSE(converted.Value().Entities().front().components.camera.has_value());
        REQUIRE(document.objects.front().components.camera.has_value());
        REQUIRE_FALSE(converted.Value().Entities()[1].components.navigationSurface.has_value());
        REQUIRE_FALSE(converted.Value().Entities()[1].components.navigationModifier.has_value());
        REQUIRE_FALSE(converted.Value().Entities()[1].components.navigationLink.has_value());
        REQUIRE_FALSE(converted.Value().Entities()[1].components.navigationAgent.has_value());
        REQUIRE(document.objects[1].components.navigationLink.has_value());
    }

    TEST_CASE("Legacy trigger volumes migrate to explicit static sensor physics producers", "[unit][editor][physics]") {
        using namespace Horo;
        using namespace Horo::Editor;

        const std::array shapes{
            Runtime::ColliderShapeType::Box,
            Runtime::ColliderShapeType::Sphere,
            Runtime::ColliderShapeType::Capsule,
            Runtime::ColliderShapeType::StaticPlane,
        };
        for (const Runtime::ColliderShapeType shape : shapes) {
            const SceneDocumentSnapshot document{
                .revision = DocumentRevision{1},
                .state = DocumentStateId{1},
                .objects = {SceneObjectSnapshot{.id = SceneObjectId{27},
                                                .name = "Legacy Trigger",
                                                .components = {.triggerVolume = Runtime::TriggerVolumeComponent{shape}}}},
            };

            const auto converted = ConvertSceneDocumentToRuntime(document, Runtime::SceneDefinitionId{1});
            REQUIRE(converted.HasValue());
            REQUIRE(document.objects.front().components.triggerVolume.has_value());
            const auto &components = converted.Value().Entities().front().components;
            REQUIRE(components.rigidBody.has_value());
            REQUIRE(components.rigidBody->motion == Runtime::AuthoredPhysicsMotionType::Static);
            REQUIRE(std::holds_alternative<Runtime::AuthoredPhysicsNoMass>(components.rigidBody->mass));
            REQUIRE(components.colliders.size() == 1);
            const auto &collider = components.colliders.front();
            REQUIRE(collider.sensor);
            REQUIRE(collider.body.object == Runtime::SceneObjectId{27});
            REQUIRE(collider.body.body == components.rigidBody->body);
            REQUIRE((collider.localPose.translation == Math::Vec3{} && collider.localPose.rotation == Math::Quaternion::Identity() &&
                     collider.scale == Math::Vec3{1.0F, 1.0F, 1.0F}));
            REQUIRE(Runtime::ValidateRigidBodyComponent(*components.rigidBody).HasValue());
            REQUIRE(Runtime::ValidateColliderComponent(collider).HasValue());

            const auto &analytic = std::get<Runtime::PhysicsAnalyticCollider>(collider.source);
            switch (shape) {
                case Runtime::ColliderShapeType::Box:
                    REQUIRE(std::holds_alternative<Runtime::PhysicsBoxCollider>(analytic));
                    break;
                case Runtime::ColliderShapeType::Sphere:
                    REQUIRE(std::holds_alternative<Runtime::PhysicsSphereCollider>(analytic));
                    break;
                case Runtime::ColliderShapeType::Capsule:
                    REQUIRE(std::holds_alternative<Runtime::PhysicsCapsuleCollider>(analytic));
                    break;
                case Runtime::ColliderShapeType::StaticPlane:
                    REQUIRE(std::holds_alternative<Runtime::PhysicsStaticPlaneCollider>(analytic));
                    break;
            }
        }
    }

    TEST_CASE("Disabled and conflicting legacy trigger volumes have explicit conversion outcomes", "[unit][editor][physics]") {
        using namespace Horo;
        using namespace Horo::Editor;

        SceneDocumentSnapshot disabled{
            .revision = DocumentRevision{1},
            .state = DocumentStateId{1},
            .objects = {SceneObjectSnapshot{.id = SceneObjectId{1},
                                            .name = "Disabled Trigger",
                                            .components = {.triggerVolume =
                                                               Runtime::TriggerVolumeComponent{.shape = Runtime::ColliderShapeType::Sphere,
                                                                                               .enabled = false}}}},
        };
        const auto disabledRuntime = ConvertSceneDocumentToRuntime(disabled, Runtime::SceneDefinitionId{1});
        REQUIRE(disabledRuntime.HasValue());
        REQUIRE_FALSE(disabledRuntime.Value().Entities().front().components.rigidBody.has_value());
        REQUIRE(disabledRuntime.Value().Entities().front().components.colliders.empty());

        SceneDocumentSnapshot conflict = disabled;
        conflict.objects.front().components.triggerVolume->enabled = true;
        conflict.objects.front().components.rigidBody = Runtime::RigidBodyComponent{.id = {9}, .body = {10}};
        const auto conflictingRuntime = ConvertSceneDocumentToRuntime(conflict, Runtime::SceneDefinitionId{1});
        REQUIRE(conflictingRuntime.HasError());
        REQUIRE(conflictingRuntime.ErrorValue().code.Value() == "scene_conversion.trigger_volume_canonical_conflict");

        SceneDocumentSnapshot malformed = disabled;
        malformed.objects.front().components.triggerVolume->enabled = true;
        malformed.objects.front().components.triggerVolume->shape = static_cast<Runtime::ColliderShapeType>(255);
        const auto malformedRuntime = ConvertSceneDocumentToRuntime(malformed, Runtime::SceneDefinitionId{1});
        REQUIRE(malformedRuntime.HasError());
        REQUIRE(malformedRuntime.ErrorValue().code.Value() == "scene_conversion.trigger_volume_schema_unsupported");
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
        REQUIRE(commands.Execute(SetSceneNavigationAgentCommand{regionObject.Value().object, NavigationAgent()}).HasValue());

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
        REQUIRE(runtime.Value().Entities()[1].components.navigationAgent->profile ==
                Navigation::NavigationAgentProfileId::Create(3).Value());

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
        components.navigationAgent = NavigationAgent();
        components.aiAgent = Horo::AI::AiAgentComponent{.agent = Horo::AI::AgentId::Create(41).Value()};
        components.aiController =
            Horo::AI::AiControllerComponent{.controller = Horo::AI::ControllerTypeId::Create(51).Value(),
                                            .decisionAsset = Horo::AI::DecisionGraphAssetId::Create(61).Value(),
                                            .blackboardSchema = Horo::AI::BlackboardSchemaId::Create(71).Value(),
                                            .requiredCapabilities = Horo::AI::AiCapabilitySet::Of(Horo::AI::AiCapability::Behavior)};
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
        REQUIRE(duplicated.navigationAgent == components.navigationAgent);
        REQUIRE(duplicated.aiAgent->agent != components.aiAgent->agent);
        REQUIRE(duplicated.aiAgent->agent.Value() == 42);
        REQUIRE(duplicated.aiAgent->schemaVersion == components.aiAgent->schemaVersion);
        REQUIRE(duplicated.aiAgent->startupPolicy == components.aiAgent->startupPolicy);
        REQUIRE(duplicated.aiAgent->enabled == components.aiAgent->enabled);
        REQUIRE(duplicated.aiController == components.aiController);
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

    TEST_CASE("Missing gameplay component payloads stay opaque and repair commands are undoable", "[unit][editor][gameplay]") {
        using namespace Horo;
        using namespace Horo::Editor;

        const Gameplay::ComponentTypeId typeId = Gameplay::ComponentTypeId::Parse("game.tests.removed_component").Value();
        const Gameplay::SerializedComponent preserved{.typeId = typeId,
                                                      .schemaVersion = 1,
                                                      .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                                      .payload = {std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}}};

        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Gameplay Actor"});
        REQUIRE(created.HasValue());
        REQUIRE(commands.Execute(SetSceneObjectGameplayComponentCommand{created.Value().object, preserved}).HasValue());
        REQUIRE(document.Objects().front().components.gameplayComponents == std::vector{preserved});

        Gameplay::ComponentRegistry missing;
        REQUIRE(missing.Freeze().HasValue());
        const SceneGameplayInspection missingInspection = InspectSceneGameplayComponents(document.Objects(), missing);
        REQUIRE(missingInspection.issues.size() == 1);
        REQUIRE(missingInspection.issues.front().object == created.Value().object);
        REQUIRE(missingInspection.issues.front().typeId == typeId);
        REQUIRE(missingInspection.issues.front().status == Gameplay::ComponentInspectionStatus::MissingDescriptor);

        const auto converted = ConvertSceneDocumentToRuntime(document.Snapshot(), Runtime::SceneDefinitionId{1});
        REQUIRE(converted.HasValue());
        REQUIRE(converted.Value().Entities().front().components.gameplayComponents == std::vector{preserved});

        REQUIRE(commands.Execute(RemoveSceneObjectGameplayComponentCommand{created.Value().object, typeId}).HasValue());
        REQUIRE(document.Objects().front().components.gameplayComponents.empty());
        REQUIRE(commands.Undo().HasValue());
        REQUIRE(document.Objects().front().components.gameplayComponents == std::vector{preserved});
        REQUIRE(commands.Redo().HasValue());
        REQUIRE(document.Objects().front().components.gameplayComponents.empty());
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

    TEST_CASE("Scene document load rejects duplicate identities and missing parents atomically", "[unit][editor][persistence]") {
        using namespace Horo::Editor;

        SceneDocument document;
        REQUIRE((document.LoadSaved({SceneObjectSnapshot{.id = SceneObjectId{1}, .name = "Baseline"}}).HasValue()));
        const DocumentRevision revision = document.Revision();
        const DocumentStateId state = document.State();

        std::vector<SceneObjectSnapshot> duplicateIds{
            SceneObjectSnapshot{.id = SceneObjectId{2}, .name = "First"},
            SceneObjectSnapshot{.id = SceneObjectId{2}, .name = "Second"},
        };
        REQUIRE((document.LoadSaved(std::move(duplicateIds)).HasError()));

        std::vector<SceneObjectSnapshot> missingParent{
            SceneObjectSnapshot{.id = SceneObjectId{2}, .parent = SceneObjectId{99}, .name = "Orphan"},
        };
        REQUIRE((document.LoadSaved(std::move(missingParent)).HasError()));
        REQUIRE((document.Revision() == revision));
        REQUIRE((document.State() == state));
        REQUIRE_FALSE((document.IsDirty()));
        REQUIRE((document.Objects().size() == 1));
        REQUIRE((document.Objects().front().id == SceneObjectId{1}));
        REQUIRE((document.Objects().front().name == "Baseline"));
    }

}  // namespace
