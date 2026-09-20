#include "editor/screens/workspace/panels/inspector/InspectorEditSession.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    [[nodiscard]] Horo::Editor::SceneObject MakeCameraObject(const Horo::Editor::SceneObjectId id) {
        Horo::Editor::SceneObject object{
            .id = id,
            .name = "Camera",
            .kind = Horo::Editor::SceneObjectKind::Camera,
        };
        object.components.camera = Horo::Runtime::CameraComponent{};
        return object;
    }

    [[nodiscard]] Horo::Editor::SceneObject MakeLightObject(const Horo::Editor::SceneObjectId id) {
        Horo::Editor::SceneObject object{
            .id = id,
            .name = "Light",
            .kind = Horo::Editor::SceneObjectKind::Light,
        };
        object.components.light = Horo::Runtime::LightComponent{.kind = Horo::Runtime::LightKind::Point};
        return object;
    }
}  // namespace

TEST_CASE("Inspector edit session reduces name edits without ImGui", "[unit][editor]") {
    using namespace Horo::Editor;
    InspectorEditSession session;
    const SceneObject object = MakeCameraObject(SceneObjectId{11});
    REQUIRE((session.BeginObject(object, DocumentRevision{3}).command == EditorWorkspaceViewCommand::None));

    session.Draft().name = "Gameplay Camera";
    const EditorWorkspaceViewCommandData rename = session.ApplyNameEdit(InspectorNameEdit{.committed = true}, object, true);
    REQUIRE((rename.command == EditorWorkspaceViewCommand::UpdateObjectName));
    REQUIRE((rename.objectPayload == object.id));
    REQUIRE((rename.stringPayload == "Gameplay Camera"));

    session.Draft().name.clear();
    REQUIRE((session.ApplyNameEdit(InspectorNameEdit{.committed = true}, object, true).command == EditorWorkspaceViewCommand::None));
    static_cast<void>(session.ApplyNameEdit(InspectorNameEdit{.cancelled = true}, object, true));
    REQUIRE((session.Draft().name == object.name));
}

TEST_CASE("Inspector edit session owns transform preview lifecycle", "[unit][editor]") {
    using namespace Horo::Editor;
    InspectorEditSession session;
    const SceneObject object = MakeCameraObject(SceneObjectId{21});
    static_cast<void>(session.BeginObject(object, DocumentRevision{1}));

    session.Draft().position[0] = 4.0F;
    const EditorWorkspaceViewCommandData preview = session.ApplyTransformEdit(
        InspectorTransformEdit{
            .changed = true,
            .changedAxes = {.position = {true, false, false}},
        },
        true);
    REQUIRE((preview.command == EditorWorkspaceViewCommand::PreviewObjectTransform));
    REQUIRE((session.HasTransformPreview()));
    REQUIRE((preview.transformUpdates->size() == 1));
    REQUIRE((preview.transformUpdates->front().localTransform.translation.x == 4.0F));

    const EditorWorkspaceViewCommandData cancelled = session.ApplyTransformEdit(InspectorTransformEdit{.cancelRequested = true}, true);
    REQUIRE((cancelled.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview));
    REQUIRE((!session.HasTransformPreview()));
    REQUIRE((session.Draft().position[0] == object.localTransform.translation.x));

    session.Draft().position[1] = 6.0F;
    const EditorWorkspaceViewCommandData committed = session.ApplyTransformEdit(
        InspectorTransformEdit{
            .changed = true,
            .committed = true,
            .changedAxes = {.position = {false, true, false}},
        },
        true);
    REQUIRE((committed.command == EditorWorkspaceViewCommand::CommitObjectTransform));
    REQUIRE((committed.transformUpdates->front().localTransform.translation.y == 6.0F));

    const EditorWorkspaceViewCommandData reset = session.ApplyTransformEdit(InspectorTransformEdit{.resetRequested = true}, true);
    REQUIRE((reset.command == EditorWorkspaceViewCommand::CommitObjectTransform));
    REQUIRE((reset.transformUpdates->front().localTransform.translation == Horo::Math::Vec3{}));
    REQUIRE((reset.transformUpdates->front().localTransform.scale == Horo::Math::Vec3{1.0F, 1.0F, 1.0F}));
}

TEST_CASE("Inspector edit session validates Camera edits and selection reconciliation", "[unit][editor]") {
    using namespace Horo::Editor;
    InspectorEditSession session;
    const SceneObject first = MakeCameraObject(SceneObjectId{31});
    const SceneObject second = MakeCameraObject(SceneObjectId{32});
    static_cast<void>(session.BeginObject(first, DocumentRevision{1}));

    session.Draft().camera->nearPlane = 0.5F;
    const EditorWorkspaceViewCommandData cameraCommand = session.ApplyCameraEdit(InspectorCameraEdit{.committed = true}, first, true);
    REQUIRE((cameraCommand.command == EditorWorkspaceViewCommand::UpdateCameraComponent));
    REQUIRE((cameraCommand.cameraPayload->nearPlane == 0.5F));

    session.Draft().camera->enabled = false;
    const EditorWorkspaceViewCommandData disableCommand = session.ApplyCameraEdit(InspectorCameraEdit{.committed = true}, first, true);
    REQUIRE((disableCommand.command == EditorWorkspaceViewCommand::UpdateCameraComponent));
    REQUIRE_FALSE(disableCommand.cameraPayload->enabled);

    session.Draft().camera->farPlane = 0.25F;
    REQUIRE((!session.IsCameraValid()));
    REQUIRE((session.ApplyCameraEdit(InspectorCameraEdit{.committed = true}, first, true).command == EditorWorkspaceViewCommand::None));

    session.Draft().position[0] = 2.0F;
    static_cast<void>(session.ApplyTransformEdit(
        InspectorTransformEdit{
            .changed = true,
            .changedAxes = {.position = {true, false, false}},
        },
        true));
    const EditorWorkspaceViewCommandData selectionChange = session.BeginObject(second, DocumentRevision{1});
    REQUIRE((selectionChange.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview));
    REQUIRE((session.Draft().object == second.id));
    REQUIRE((session.IsCameraValid()));
}

TEST_CASE("Inspector edit session validates typed Light edits", "[unit][editor]") {
    using namespace Horo::Editor;
    InspectorEditSession session;
    const SceneObject first = MakeLightObject(SceneObjectId{35});
    const SceneObject second = MakeLightObject(SceneObjectId{36});
    static_cast<void>(session.BeginObject(first, DocumentRevision{1}));

    session.Draft().light->kind = Horo::Runtime::LightKind::Spot;
    session.Draft().light->intensity = 4.0F;
    session.Draft().light->range = 25.0F;
    session.Draft().light->innerConeRadians = 0.25F;
    session.Draft().light->outerConeRadians = 0.75F;
    const EditorWorkspaceViewCommandData lightPreview = session.ApplyLightEdit(InspectorLightEdit{.changed = true}, first, true);
    REQUIRE((lightPreview.command == EditorWorkspaceViewCommand::PreviewLightComponent));
    REQUIRE((lightPreview.lightPayload == session.Draft().light));
    REQUIRE((session.HasLightPreview()));

    const EditorWorkspaceViewCommandData lightCommand = session.ApplyLightEdit(InspectorLightEdit{.committed = true}, first, true);
    REQUIRE((lightCommand.command == EditorWorkspaceViewCommand::UpdateLightComponent));
    REQUIRE((lightCommand.lightPayload == session.Draft().light));
    REQUIRE((!session.HasLightPreview()));

    session.Draft().light->intensity = -1.0F;
    REQUIRE((!session.IsLightValid()));
    REQUIRE((session.ApplyLightEdit(InspectorLightEdit{.committed = true}, first, true).command == EditorWorkspaceViewCommand::None));

    const std::array objects{first, second};
    const std::array selected{first.id, second.id};
    static_cast<void>(session.BeginSelection(objects, selected, first.id, DocumentRevision{2}));
    REQUIRE((!session.Draft().light.has_value()));
    REQUIRE((!session.IsLightValid()));
}

TEST_CASE("Inspector Light preview cancels and restores its authored draft", "[unit][editor]") {
    using namespace Horo::Editor;
    InspectorEditSession session;
    const SceneObject first = MakeLightObject(SceneObjectId{37});
    const SceneObject second = MakeLightObject(SceneObjectId{38});
    static_cast<void>(session.BeginObject(first, DocumentRevision{1}));

    session.Draft().light->intensity = 8.0F;
    REQUIRE((session.ApplyLightEdit(InspectorLightEdit{.changed = true}, first, true).command ==
             EditorWorkspaceViewCommand::PreviewLightComponent));
    const EditorWorkspaceViewCommandData cancelled = session.ApplyLightEdit(InspectorLightEdit{.cancelRequested = true}, first, true);
    REQUIRE((cancelled.command == EditorWorkspaceViewCommand::CancelLightComponentPreview));
    REQUIRE((session.Draft().light == first.components.light));
    REQUIRE((!session.HasLightPreview()));

    session.Draft().light->range = 42.0F;
    static_cast<void>(session.ApplyLightEdit(InspectorLightEdit{.changed = true}, first, true));
    const EditorWorkspaceViewCommandData selectionChanged = session.BeginObject(second, DocumentRevision{1});
    REQUIRE((selectionChanged.command == EditorWorkspaceViewCommand::CancelLightComponentPreview));
    REQUIRE((session.Draft().object == second.id));
    REQUIRE((!session.HasLightPreview()));
}

TEST_CASE("Inspector edit session projects mixed axes and batches selected transforms", "[unit][editor]") {
    using namespace Horo::Editor;
    InspectorEditSession session;
    SceneObject first = MakeCameraObject(SceneObjectId{41});
    first.localTransform.translation = {1.0F, 2.0F, 3.0F};
    first.localTransform.scale = {1.0F, 2.0F, 1.0F};
    SceneObject second = MakeCameraObject(SceneObjectId{42});
    second.localTransform.translation = {4.0F, 2.0F, 3.0F};
    second.localTransform.scale = {1.0F, 3.0F, 1.0F};
    const std::array objects{first, second};
    const std::array selected{first.id, second.id};

    REQUIRE((session.BeginSelection(objects, selected, first.id, DocumentRevision{5}).command == EditorWorkspaceViewCommand::None));
    REQUIRE((session.Draft().selectedObjectCount == 2));
    REQUIRE((session.Draft().mixed.position == std::array{true, false, false}));
    REQUIRE((session.Draft().mixed.scale == std::array{false, true, false}));
    REQUIRE((!session.Draft().camera.has_value()));

    session.Draft().position[0] = 8.0F;
    session.Draft().position[1] = 9.0F;
    session.Draft().scale[1] = 2.5F;
    const EditorWorkspaceViewCommandData preview = session.ApplyTransformEdit(
        InspectorTransformEdit{
            .changed = true,
            .changedAxes =
                {
                    .position = {true, true, false},
                    .scale = {false, true, false},
                },
        },
        true);
    REQUIRE((preview.command == EditorWorkspaceViewCommand::PreviewObjectTransform));
    REQUIRE((preview.transformUpdates->size() == 2));
    REQUIRE((preview.transformUpdates->at(0).localTransform.translation == Horo::Math::Vec3{8.0F, 9.0F, 3.0F}));
    REQUIRE((preview.transformUpdates->at(1).localTransform.translation == Horo::Math::Vec3{11.0F, 9.0F, 3.0F}));
    REQUIRE((preview.transformUpdates->at(0).localTransform.scale == Horo::Math::Vec3{1.0F, 2.5F, 1.0F}));
    REQUIRE((preview.transformUpdates->at(1).localTransform.scale == Horo::Math::Vec3{1.0F, 3.5F, 1.0F}));
    REQUIRE((!session.Draft().mixed.position[0]));

    const EditorWorkspaceViewCommandData commit = session.ApplyTransformEdit(InspectorTransformEdit{.committed = true}, true);
    REQUIRE((commit.command == EditorWorkspaceViewCommand::CommitObjectTransform));
    REQUIRE((commit.transformUpdates == preview.transformUpdates));
    REQUIRE((!session.HasTransformPreview()));
}

TEST_CASE("Inspector multi-selection preview cancels on selection change", "[unit][editor]") {
    using namespace Horo::Editor;
    InspectorEditSession session;
    const SceneObject first = MakeCameraObject(SceneObjectId{51});
    const SceneObject second = MakeCameraObject(SceneObjectId{52});
    const std::array objects{first, second};
    const std::array selected{first.id, second.id};
    static_cast<void>(session.BeginSelection(objects, selected, first.id, DocumentRevision{1}));
    session.Draft().scale[2] = 2.0F;
    static_cast<void>(session.ApplyTransformEdit(
        InspectorTransformEdit{
            .changed = true,
            .changedAxes = {.scale = {false, false, true}},
        },
        true));

    const std::array nextSelection{second.id};
    const EditorWorkspaceViewCommandData changed = session.BeginSelection(objects, nextSelection, second.id, DocumentRevision{1});
    REQUIRE((changed.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview));
    REQUIRE((session.Draft().selectedObjectCount == 1));
    REQUIRE((session.Draft().object == second.id));
    REQUIRE((!session.HasTransformPreview()));
}

TEST_CASE("Inspector multi-selection refreshes when only the primary object changes", "[unit][editor]") {
    using namespace Horo::Editor;
    InspectorEditSession session;
    SceneObject first = MakeCameraObject(SceneObjectId{61});
    first.localTransform.translation.x = 1.0F;
    SceneObject second = MakeCameraObject(SceneObjectId{62});
    second.localTransform.translation.x = 4.0F;
    const std::array objects{first, second};
    const std::array selected{first.id, second.id};

    static_cast<void>(session.BeginSelection(objects, selected, first.id, DocumentRevision{1}));
    REQUIRE((session.Draft().object == first.id));
    REQUIRE((session.Draft().position[0] == 1.0F));

    session.Draft().position[0] = 3.0F;
    static_cast<void>(session.ApplyTransformEdit(
        InspectorTransformEdit{
            .changed = true,
            .changedAxes = {.position = {true, false, false}},
        },
        true));
    const EditorWorkspaceViewCommandData primaryChanged = session.BeginSelection(objects, selected, second.id, DocumentRevision{1});

    REQUIRE((primaryChanged.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview));
    REQUIRE((session.Draft().object == second.id));
    REQUIRE((session.Draft().position[0] == 4.0F));
    REQUIRE((!session.HasTransformPreview()));
}

TEST_CASE("Inspector no-op transform commit does not leak edited axes into the next interaction", "[unit][editor]") {
    using namespace Horo::Editor;
    InspectorEditSession session;
    const SceneObject object = MakeCameraObject(SceneObjectId{71});
    static_cast<void>(session.BeginObject(object, DocumentRevision{1}));

    session.Draft().position[0] = 2.0F;
    static_cast<void>(session.ApplyTransformEdit(
        InspectorTransformEdit{
            .changed = true,
            .changedAxes = {.position = {true, false, false}},
        },
        true));
    session.Draft().position[0] = object.localTransform.translation.x;
    const EditorWorkspaceViewCommandData noOpCommit = session.ApplyTransformEdit(InspectorTransformEdit{.committed = true}, true);
    REQUIRE((noOpCommit.command == EditorWorkspaceViewCommand::CommitObjectTransform));

    REQUIRE((session.ApplyTransformEdit(InspectorTransformEdit{.committed = true}, true).command == EditorWorkspaceViewCommand::None));
}

TEST_CASE("Inspector edit session validates typed TriggerVolume and AudioSource edits", "[unit][editor]") {
    using namespace Horo::Editor;
    SceneObject object = MakeCameraObject(SceneObjectId{81});
    object.components.triggerVolume = Horo::Runtime::TriggerVolumeComponent{};
    object.components.audioSource = Horo::Runtime::AudioSourceComponent{};
    InspectorEditSession session;
    static_cast<void>(session.BeginObject(object, DocumentRevision{1}));

    session.Draft().triggerVolume->shape = Horo::Runtime::ColliderShapeType::Sphere;
    const EditorWorkspaceViewCommandData trigger =
        session.ApplyTriggerVolumeEdit(InspectorTriggerVolumeEdit{.committed = true}, object, true);
    REQUIRE((trigger.command == EditorWorkspaceViewCommand::UpdateTriggerVolumeComponent));
    REQUIRE((trigger.triggerVolumePayload->shape == Horo::Runtime::ColliderShapeType::Sphere));

    session.Draft().audioSource->playback.gain = 2.0F;
    const EditorWorkspaceViewCommandData audio = session.ApplyAudioSourceEdit(InspectorAudioSourceEdit{.committed = true}, object, true);
    REQUIRE((audio.command == EditorWorkspaceViewCommand::UpdateAudioSourceComponent));
    REQUIRE((audio.audioSourcePayload->playback.gain == 2.0F));

    session.Draft().audioSource->playback.gain = -1.0F;
    REQUIRE((!session.IsAudioSourceValid()));
    REQUIRE((session.ApplyAudioSourceEdit(InspectorAudioSourceEdit{.committed = true}, object, true).command ==
             EditorWorkspaceViewCommand::None));
}
