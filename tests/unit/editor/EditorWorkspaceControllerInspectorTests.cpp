#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    namespace Math = Math;
    using TestWorkspaceController = HoroEditorWorkspaceControllerPolicyTests::FocusedWorkspaceController;
    using namespace HoroEditorWorkspaceControllerFilesystemTests;

    /** @brief Verify Preview Commit And Undo. */
    void VerifyPreviewCommitAndUndo(TestWorkspaceController &controller, SceneObjectId object, DocumentRevision initialRevision,
                                    const Math::Transform &edited, const std::vector<SceneDocumentChangedEvent> &documentEvents,
                                    const std::vector<ViewportChangedEvent> &viewportEvents) {
        EditorWorkspaceViewCommandData commit;
        commit.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        commit.objectPayload = object;
        commit.transformPayload = edited;
        controller.ProcessCommand(commit);
        REQUIRE((controller.ViewModel().objects.front().localTransform == edited));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().isDirty && controller.ViewModel().canUndo));
        REQUIRE((documentEvents.size() == 1 && documentEvents.front().kind == DocumentChangeKind::TransformChanged));
        REQUIRE((!controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));
        REQUIRE((controller.CurrentViewportRevision() == ViewportRevision{2}));
        REQUIRE((viewportEvents.size() == 2 && viewportEvents.back().kind == ViewportChangeKind::ScenePreviewChanged));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        REQUIRE((!controller.ViewModel().isDirty));
    }

    TEST_CASE("Gizmo Preview Is Transient And Commit Creates One Undoable Document Change", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;
        Math::Transform edited = controller.ViewModel().objects.front().localTransform;
        edited.translation = {1.0F, 2.0F, -0.5F};
        std::vector<SceneDocumentChangedEvent> documentEvents;
        std::vector<ViewportChangedEvent> viewportEvents;
        auto documentSubscription =
            controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&documentEvents](const SceneDocumentChangedEvent &event) {
            documentEvents.push_back(event);
        });
        auto viewportSubscription =
            controller.DataBus().Subscribe<ViewportChangedEvent>([&viewportEvents](const ViewportChangedEvent &event) {
            viewportEvents.push_back(event);
        });

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectPayload = object;
        controller.ProcessCommand(select);

        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.objectPayload = object;
        preview.transformPayload = edited;
        controller.ProcessCommand(preview);
        REQUIRE((controller.ViewModel().objects.front().localTransform != edited));
        REQUIRE((controller.ViewModel().documentRevision == initialRevision));
        REQUIRE((!controller.ViewModel().isDirty && !controller.ViewModel().canUndo));
        const Math::Vec3 previewOrigin = Math::TransformPoint(controller.ViewportScene().instances.front().localToWorld, {});
        REQUIRE((previewOrigin == edited.translation));
        REQUIRE((controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));
        REQUIRE((Math::TransformPoint(*controller.ViewModel().primarySelectionPreviewWorldTransform, {}) == edited.translation));
        REQUIRE((documentEvents.empty()));
        REQUIRE((viewportEvents.size() == 1 && viewportEvents.front().kind == ViewportChangeKind::ScenePreviewChanged));

        VerifyPreviewCommitAndUndo(controller, object, initialRevision, edited, documentEvents, viewportEvents);

        static_cast<void>(documentSubscription);
        static_cast<void>(viewportSubscription);
    }

    TEST_CASE("Inspector Rename Updates Projection Once And Remains Undoable", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const std::string originalName = controller.ViewModel().objects.front().name;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;

        EditorWorkspaceViewCommandData rename;
        rename.command = EditorWorkspaceViewCommand::UpdateObjectName;
        rename.objectPayload = object;
        rename.stringPayload = "Inspector Hero";
        controller.ProcessCommand(rename);

        REQUIRE((controller.ViewModel().objects.front().name == "Inspector Hero"));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().canUndo));

        controller.ProcessCommand(rename);
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.front().name == originalName));
    }

    TEST_CASE("Inspector Camera Update Uses Typed Document History", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData createCamera;
        createCamera.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createCamera.primitivePayload = Runtime::PrimitiveId{"primitive.object.camera"};
        controller.ProcessCommand(createCamera);

        const SceneObject camera = controller.ViewModel().objects.back();
        REQUIRE((camera.kind == SceneObjectKind::Camera));
        REQUIRE((camera.components.camera.has_value()));
        const Runtime::CameraComponent original = *camera.components.camera;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;

        Runtime::CameraComponent edited = original;
        edited.projection = Runtime::CameraProjection::Orthographic;
        edited.orthographicHeight = 24.0F;
        edited.nearPlane = 0.5F;
        edited.farPlane = 5000.0F;

        EditorWorkspaceViewCommandData update;
        update.command = EditorWorkspaceViewCommand::UpdateCameraComponent;
        update.objectPayload = camera.id;
        update.cameraPayload = edited;
        controller.ProcessCommand(update);

        const auto updated = std::ranges::find(controller.ViewModel().objects, camera.id, &SceneObject::id);
        REQUIRE((updated != controller.ViewModel().objects.end()));
        REQUIRE((updated->components.camera == edited));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().canUndo));

        controller.ProcessCommand(update);
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        const auto restored = std::ranges::find(controller.ViewModel().objects, camera.id, &SceneObject::id);
        REQUIRE((restored != controller.ViewModel().objects.end()));
        REQUIRE((restored->components.camera == original));
    }

    TEST_CASE("Inspector Light Update Uses Typed Document History", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData createLight;
        createLight.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createLight.primitivePayload = Runtime::PrimitiveId{"primitive.object.light_point"};
        controller.ProcessCommand(createLight);

        const SceneObject light = controller.ViewModel().objects.back();
        REQUIRE((light.kind == SceneObjectKind::Light));
        REQUIRE((light.components.light.has_value()));
        const Runtime::LightComponent original = *light.components.light;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;

        Runtime::LightComponent edited = original;
        edited.kind = Runtime::LightKind::Spot;
        edited.color = {0.4F, 0.6F, 0.8F};
        edited.intensity = 5.0F;
        edited.range = 30.0F;
        edited.innerConeRadians = 0.25F;
        edited.outerConeRadians = 0.75F;

        EditorWorkspaceViewCommandData update;
        update.command = EditorWorkspaceViewCommand::UpdateLightComponent;
        update.objectPayload = light.id;
        update.lightPayload = edited;
        controller.ProcessCommand(update);

        const auto updated = std::ranges::find(controller.ViewModel().objects, light.id, &SceneObject::id);
        REQUIRE((updated != controller.ViewModel().objects.end()));
        REQUIRE((updated->components.light == edited));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().canUndo));

        controller.ProcessCommand(update);
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        const auto restored = std::ranges::find(controller.ViewModel().objects, light.id, &SceneObject::id);
        REQUIRE((restored != controller.ViewModel().objects.end()));
        REQUIRE((restored->components.light == original));
    }

    TEST_CASE("Inspector Optional Components Add Update And Remove Through Typed History", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;

        EditorWorkspaceViewCommandData addTrigger;
        addTrigger.command = EditorWorkspaceViewCommand::AddComponentToObject;
        addTrigger.objectPayload = object;
        addTrigger.componentTypePayload = ComponentType::TriggerVolume;
        controller.ProcessCommand(addTrigger);
        REQUIRE((controller.ViewModel().objects.front().components.triggerVolume.has_value()));

        Runtime::TriggerVolumeComponent trigger = *controller.ViewModel().objects.front().components.triggerVolume;
        trigger.shape = Runtime::ColliderShapeType::Sphere;
        EditorWorkspaceViewCommandData updateTrigger;
        updateTrigger.command = EditorWorkspaceViewCommand::UpdateTriggerVolumeComponent;
        updateTrigger.objectPayload = object;
        updateTrigger.triggerVolumePayload = trigger;
        controller.ProcessCommand(updateTrigger);
        REQUIRE((controller.ViewModel().objects.front().components.triggerVolume == trigger));

        EditorWorkspaceViewCommandData addAudio;
        addAudio.command = EditorWorkspaceViewCommand::AddComponentToObject;
        addAudio.objectPayload = object;
        addAudio.componentTypePayload = ComponentType::AudioSource;
        controller.ProcessCommand(addAudio);
        Runtime::AudioSourceComponent audio = *controller.ViewModel().objects.front().components.audioSource;
        audio.playback.gain = 1.5F;
        EditorWorkspaceViewCommandData updateAudio;
        updateAudio.command = EditorWorkspaceViewCommand::UpdateAudioSourceComponent;
        updateAudio.objectPayload = object;
        updateAudio.audioSourcePayload = audio;
        controller.ProcessCommand(updateAudio);
        REQUIRE((controller.ViewModel().objects.front().components.audioSource == audio));

        EditorWorkspaceViewCommandData removeTrigger;
        removeTrigger.command = EditorWorkspaceViewCommand::RemoveComponentFromObject;
        removeTrigger.objectPayload = object;
        removeTrigger.componentTypePayload = ComponentType::TriggerVolume;
        controller.ProcessCommand(removeTrigger);
        REQUIRE((!controller.ViewModel().objects.front().components.triggerVolume.has_value()));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 5));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.front().components.triggerVolume == trigger));
    }

    TEST_CASE("Inspector Light Preview Updates Viewport Without Mutating Document", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData createLight;
        createLight.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createLight.primitivePayload = Runtime::PrimitiveId{"primitive.object.light_point"};
        controller.ProcessCommand(createLight);
        const SceneObject light = controller.ViewModel().objects.back();
        const Runtime::LightComponent original = *light.components.light;
        const DocumentRevision originalRevision = controller.ViewModel().documentRevision;

        Runtime::LightComponent edited = original;
        edited.intensity = 7.0F;
        edited.range = 35.0F;
        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewLightComponent;
        preview.objectPayload = light.id;
        preview.lightPayload = edited;
        controller.ProcessCommand(preview);

        REQUIRE((controller.ViewModel().documentRevision == originalRevision));
        REQUIRE((controller.ViewModel().objects.back().components.light == original));
        REQUIRE((controller.ViewportScene().lights.back().intensity == 7.0F));
        REQUIRE((controller.ViewportScene().lights.back().range == 35.0F));
        REQUIRE((controller.ViewModel().viewportLights.back().object == light.id));
        REQUIRE((controller.ViewModel().viewportLights.back().light.intensity == 7.0F));
        REQUIRE((controller.ViewModel().viewportLights.back().light.range == 35.0F));

        EditorWorkspaceViewCommandData cancel;
        cancel.command = EditorWorkspaceViewCommand::CancelLightComponentPreview;
        controller.ProcessCommand(cancel);
        REQUIRE((controller.ViewModel().documentRevision == originalRevision));
        REQUIRE((controller.ViewportScene().lights.back().intensity == original.intensity));
        REQUIRE((controller.ViewportScene().lights.back().range == original.range));
        REQUIRE((controller.ViewModel().viewportLights.back().object == light.id));
        REQUIRE((controller.ViewModel().viewportLights.back().light.intensity == original.intensity));
        REQUIRE((controller.ViewModel().viewportLights.back().light.range == original.range));
    }

    TEST_CASE("Cancelling Gizmo Preview Restores The Exact Committed Projection", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const Math::Transform committedTransform = controller.ViewModel().objects.front().localTransform;
        const Math::Mat4 committedMatrix = controller.ViewportScene().instances.front().localToWorld;
        const DocumentRevision committedRevision = controller.ViewModel().documentRevision;
        std::vector<SceneDocumentChangedEvent> documentEvents;
        std::vector<ViewportChangedEvent> viewportEvents;
        auto documentSubscription =
            controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&documentEvents](const SceneDocumentChangedEvent &event) {
            documentEvents.push_back(event);
        });
        auto viewportSubscription =
            controller.DataBus().Subscribe<ViewportChangedEvent>([&viewportEvents](const ViewportChangedEvent &event) {
            viewportEvents.push_back(event);
        });

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectPayload = object;
        controller.ProcessCommand(select);

        Math::Transform previewTransform = committedTransform;
        previewTransform.translation = {-2.0F, 0.5F, 1.0F};
        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.objectPayload = object;
        preview.transformPayload = previewTransform;
        controller.ProcessCommand(preview);
        REQUIRE((controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));

        EditorWorkspaceViewCommandData cancel;
        cancel.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
        controller.ProcessCommand(cancel);

        REQUIRE((controller.ViewModel().objects.front().localTransform == committedTransform));
        REQUIRE((controller.ViewportScene().instances.front().localToWorld == committedMatrix));
        REQUIRE((!controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));
        REQUIRE((controller.ViewModel().documentRevision == committedRevision));
        REQUIRE((!controller.ViewModel().isDirty && !controller.ViewModel().canUndo));
        REQUIRE((documentEvents.empty()));
        REQUIRE((controller.CurrentViewportRevision() == ViewportRevision{2}));
        REQUIRE((viewportEvents.size() == 2));
        static_cast<void>(documentSubscription);
        static_cast<void>(viewportSubscription);
    }

    TEST_CASE("Idle Frames Preserve Inspector Transform And Light Previews", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObject mesh = controller.ViewModel().objects.front();
        EditorWorkspaceViewCommandData selectMesh;
        selectMesh.command = EditorWorkspaceViewCommand::SelectObject;
        selectMesh.objectPayload = mesh.id;
        controller.ProcessCommand(selectMesh);
        Math::Transform previewTransform = mesh.localTransform;
        previewTransform.translation = {3.0F, 2.0F, 1.0F};

        EditorWorkspaceViewCommandData transformPreview;
        transformPreview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        transformPreview.transformUpdates = std::vector{SceneObjectTransformUpdate{mesh.id, previewTransform}};
        controller.ProcessCommand(transformPreview);
        controller.ProcessCommand({});

        const auto meshInstance = std::ranges::find(controller.ViewportScene().instanceObjects, mesh.id);
        REQUIRE((meshInstance != controller.ViewportScene().instanceObjects.end()));
        const std::size_t meshIndex =
            static_cast<std::size_t>(std::distance(controller.ViewportScene().instanceObjects.begin(), meshInstance));
        REQUIRE((Math::TransformPoint(controller.ViewportScene().instances[meshIndex].localToWorld, {}) == previewTransform.translation));
        REQUIRE((controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));

        EditorWorkspaceViewCommandData createLight;
        createLight.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createLight.primitivePayload = Runtime::PrimitiveId{"primitive.object.light_point"};
        controller.ProcessCommand(createLight);
        const SceneObject light = controller.ViewModel().objects.back();
        Runtime::LightComponent previewLight = *light.components.light;
        previewLight.intensity = 9.0F;

        EditorWorkspaceViewCommandData lightPreview;
        lightPreview.command = EditorWorkspaceViewCommand::PreviewLightComponent;
        lightPreview.objectPayload = light.id;
        lightPreview.lightPayload = previewLight;
        controller.ProcessCommand(lightPreview);
        controller.ProcessCommand({});

        REQUIRE((controller.ViewportScene().lights.back().intensity == 9.0F));
        REQUIRE((controller.ViewModel().viewportLights.back().light.intensity == 9.0F));
    }

    TEST_CASE("Selection Change Cancels A Transient Inspector Transform Preview", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObject first = controller.ViewModel().objects.front();

        auto create = BoxCreationCommand();
        controller.ProcessCommand(create);
        const SceneObject second = controller.ViewModel().objects.back();
        const DocumentRevision committedRevision = controller.ViewModel().documentRevision;
        const bool committedDirtyState = controller.ViewModel().isDirty;
        const bool committedUndoState = controller.ViewModel().canUndo;

        EditorWorkspaceViewCommandData selectFirst;
        selectFirst.command = EditorWorkspaceViewCommand::SelectObject;
        selectFirst.objectPayload = first.id;
        controller.ProcessCommand(selectFirst);

        Math::Transform previewTransform = first.localTransform;
        previewTransform.translation = {4.0F, 2.0F, -1.0F};
        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.objectPayload = first.id;
        preview.transformPayload = previewTransform;
        controller.ProcessCommand(preview);

        const auto firstInstance = std::ranges::find(controller.ViewportScene().instanceObjects, first.id);
        REQUIRE((firstInstance != controller.ViewportScene().instanceObjects.end()));
        const std::size_t firstIndex =
            static_cast<std::size_t>(std::distance(controller.ViewportScene().instanceObjects.begin(), firstInstance));
        REQUIRE((Math::TransformPoint(controller.ViewportScene().instances[firstIndex].localToWorld, {}) == previewTransform.translation));

        EditorWorkspaceViewCommandData selectSecond;
        selectSecond.command = EditorWorkspaceViewCommand::SelectObject;
        selectSecond.objectPayload = second.id;
        controller.ProcessCommand(selectSecond);

        REQUIRE((controller.ViewModel().primarySelection == second.id));
        REQUIRE((controller.ViewModel().objects.front().localTransform == first.localTransform));
        REQUIRE(
            (Math::TransformPoint(controller.ViewportScene().instances[firstIndex].localToWorld, {}) == first.localTransform.translation));
        REQUIRE((controller.ViewModel().documentRevision == committedRevision));
        REQUIRE((controller.ViewModel().isDirty == committedDirtyState));
        REQUIRE((controller.ViewModel().canUndo == committedUndoState));
    }

    TEST_CASE("No Op Gizmo Commit Clears Preview Without Creating History", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const Math::Transform committedTransform = controller.ViewModel().objects.front().localTransform;
        Math::Transform movedTransform = committedTransform;
        movedTransform.translation = {3.0F, 0.0F, 0.0F};

        EditorWorkspaceViewCommandData previewMoved;
        previewMoved.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        previewMoved.objectPayload = object;
        previewMoved.transformPayload = movedTransform;
        controller.ProcessCommand(previewMoved);

        EditorWorkspaceViewCommandData previewRestored = previewMoved;
        previewRestored.transformPayload = committedTransform;
        controller.ProcessCommand(previewRestored);

        EditorWorkspaceViewCommandData commit;
        commit.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        commit.objectPayload = object;
        commit.transformPayload = committedTransform;
        controller.ProcessCommand(commit);

        REQUIRE((controller.ViewModel().objects.front().localTransform == committedTransform));
        REQUIRE((!controller.ViewModel().isDirty && !controller.ViewModel().canUndo));
        REQUIRE((controller.CurrentViewportRevision() == ViewportRevision{3}));
    }

    TEST_CASE("Inspector Batch Transform Previews And Commits As One Undoable Change", "[unit][editor]") {
        TestWorkspaceController controller;

        auto create = BoxCreationCommand();
        controller.ProcessCommand(create);
        REQUIRE((controller.ViewModel().objects.size() == 2));

        const SceneObject first = controller.ViewModel().objects.front();
        const SceneObject second = controller.ViewModel().objects.back();
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;
        Math::Transform firstEdited = first.localTransform;
        firstEdited.translation = {2.0F, 0.0F, 0.0F};
        Math::Transform secondEdited = second.localTransform;
        secondEdited.translation = {-3.0F, 1.0F, 0.0F};
        const std::vector updates{
            SceneObjectTransformUpdate{first.id, firstEdited},
            SceneObjectTransformUpdate{second.id, secondEdited},
        };

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectSelection = ObjectSelectionRequest{.objects = {first.id, second.id}, .primary = first.id};
        controller.ProcessCommand(select);
        REQUIRE((controller.ViewModel().selectedObjects == std::vector{first.id, second.id}));

        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.transformUpdates = updates;
        controller.ProcessCommand(preview);
        REQUIRE((controller.ViewModel().documentRevision == initialRevision));
        REQUIRE((controller.ViewModel().objects.front().localTransform == first.localTransform));
        for (const SceneObjectTransformUpdate &update : updates) {
            const auto instance = std::ranges::find(controller.ViewportScene().instanceObjects, update.object);
            REQUIRE((instance != controller.ViewportScene().instanceObjects.end()));
            const std::size_t index = static_cast<std::size_t>(std::distance(controller.ViewportScene().instanceObjects.begin(), instance));
            REQUIRE(
                (Math::TransformPoint(controller.ViewportScene().instances[index].localToWorld, {}) == update.localTransform.translation));
        }

        EditorWorkspaceViewCommandData commit;
        commit.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        commit.transformUpdates = updates;
        controller.ProcessCommand(commit);
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().objects.front().localTransform == firstEdited));
        REQUIRE((controller.ViewModel().objects.back().localTransform == secondEdited));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.front().localTransform == first.localTransform));
        REQUIRE((controller.ViewModel().objects.back().localTransform == second.localTransform));
    }

    TEST_CASE("No Op Inspector Batch Commit Clears Preview Without Advancing Document", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObject object = controller.ViewModel().objects.front();
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;
        Math::Transform moved = object.localTransform;
        moved.translation.x += 2.0F;

        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.transformUpdates = std::vector{SceneObjectTransformUpdate{object.id, moved}};
        controller.ProcessCommand(preview);

        EditorWorkspaceViewCommandData commit;
        commit.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        commit.transformUpdates = std::vector{SceneObjectTransformUpdate{object.id, object.localTransform}};
        controller.ProcessCommand(commit);

        REQUIRE((controller.ViewModel().documentRevision == initialRevision));
        REQUIRE((!controller.ViewModel().canUndo));
        REQUIRE((controller.ViewModel().objects.front().localTransform == object.localTransform));
    }
}  // namespace
