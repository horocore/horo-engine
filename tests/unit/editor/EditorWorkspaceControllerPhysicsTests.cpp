#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Explicit test owner/project identities, never production fallback bindings. */
    Assets::AssetId Identity(const std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    /** @brief Capture the actual controller projection fence for one complete body/collider creation. */
    EditorWorkspaceViewCommandData Creation(const EditorWorkspaceViewModel &view) {
        const auto id = view.objects.front().id;
        Runtime::RigidBodyComponent body{.id = {1}, .body = {10}};
        Runtime::ColliderComponent collider{.id = {2},
                                            .collider = {20},
                                            .body = {{id.value}, {10}},
                                            .collisionProfile = Physics::CollisionProfileId::FromBytes(Identity(2).Bytes()),
                                            .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), Identity(3)}}};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::UpdatePhysicsComponents;
        command.objectPayload = id;
        command.physicsPayload = PhysicsInspectorRequest{view.physicsContext, view.physicsSelectionRevision, {body, {collider}, {}}};
        return command;
    }

    /** @brief Select through the real selection authority instead of mutating its view model. */
    void Select(EditorWorkspaceController &controller, const SceneObjectId object) {
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::SelectObject;
        command.objectPayload = object;
        controller.ProcessCommand(command);
    }

    /** @brief Author a real two-child compound through the existing Inspector command path. */
    void CreateCompound(EditorWorkspaceController &controller) {
        Select(controller, controller.ViewModel().objects.front().id);
        auto creation = Creation(controller.ViewModel());
        auto second = creation.physicsPayload->values.colliders.front();
        second.id = {3};
        second.collider = {21};
        second.localPose.translation.x = 2;
        creation.physicsPayload->values.colliders.push_back(second);
        controller.ProcessCommand(creation);
        REQUIRE(controller.ViewModel().physicsEditError.empty());
        REQUIRE(controller.ViewModel().viewportColliders.size() == 2);
    }
}  // namespace

TEST_CASE("Physics Inspector controller commits once through canonical history and events", "[unit][editor][physics][inspector]") {
    Runtime::RuntimeSceneService runtime;
    EditorWorkspaceController controller{"test-project", runtime, {}, {.documentSessionOwner = Identity(1)}};
    REQUIRE(controller.ViewModel().objects.size() == 1);
    Select(controller, controller.ViewModel().objects.front().id);
    auto command = Creation(controller.ViewModel());
    std::size_t events = 0;
    auto subscription = controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&](const auto &) {
        ++events;
    });
    controller.ProcessCommand(command);
    CHECK(controller.ViewModel().physicsEditError.empty());
    CHECK(controller.ViewModel().objects.front().components.rigidBody.has_value());
    CHECK(controller.ViewModel().canUndo);
    CHECK(events == 1);
    controller.ProcessCommand(command);
    CHECK_FALSE(controller.ViewModel().physicsEditError.empty());
    CHECK(events == 1);
    EditorWorkspaceViewCommandData replay;
    replay.command = EditorWorkspaceViewCommand::UndoScene;
    controller.ProcessCommand(replay);
    CHECK_FALSE(controller.ViewModel().objects.front().components.rigidBody.has_value());
    CHECK(events == 2);
    replay.command = EditorWorkspaceViewCommand::RedoScene;
    controller.ProcessCommand(replay);
    CHECK(controller.ViewModel().objects.front().components.rigidBody.has_value());
    CHECK(events == 3);
}

TEST_CASE("Physics Inspector rejects an expired selection even if the same object is selected again",
          "[unit][editor][physics][inspector]") {
    Runtime::RuntimeSceneService runtime;
    EditorWorkspaceController controller{"test-project", runtime, {}, {.documentSessionOwner = Identity(1)}};
    const auto first = controller.ViewModel().objects.front().id;
    Select(controller, first);
    auto stale = Creation(controller.ViewModel());
    EditorWorkspaceViewCommandData clear;
    clear.command = EditorWorkspaceViewCommand::SelectObject;
    clear.objectSelection = ObjectSelectionRequest{};
    controller.ProcessCommand(clear);
    Select(controller, first);
    CHECK(controller.ViewModel().documentRevision.value == stale.physicsPayload->context.revision);
    controller.ProcessCommand(stale);
    CHECK_FALSE(controller.ViewModel().physicsEditError.empty());
    CHECK_FALSE(controller.ViewModel().objects.front().components.rigidBody.has_value());
}

TEST_CASE("Physics Inspector requires the explicit host document opening identity", "[unit][editor][physics][inspector]") {
    Runtime::RuntimeSceneService runtime;
    EditorWorkspaceController controller{"test-project", runtime, {}};
    Select(controller, controller.ViewModel().objects.front().id);
    controller.ProcessCommand(Creation(controller.ViewModel()));
    CHECK_FALSE(controller.ViewModel().physicsEditError.empty());
    CHECK_FALSE(controller.ViewModel().objects.front().components.rigidBody.has_value());
}

TEST_CASE("Collider viewport edits use the real Inspector transaction owner and one undo event", "[unit][editor][physics][collider]") {
    Runtime::RuntimeSceneService runtime;
    EditorWorkspaceController controller{"test-project", runtime, {}, {.documentSessionOwner = Identity(1)}};
    CreateCompound(controller);
    EditorWorkspaceViewCommandData selection;
    selection.command = EditorWorkspaceViewCommand::SelectPhysicsColliders;
    const auto &view = controller.ViewModel();
    selection.colliderPayload = ColliderViewportRequest{view.physicsContext,
                                                        view.colliderSelectionRevision,
                                                        {view.viewportColliders[0].target, view.viewportColliders[1].target},
                                                        {}};
    controller.ProcessCommand(selection);
    REQUIRE(controller.ViewModel().selectedColliders.size() == 2);
    const auto before = controller.ViewModel().documentRevision;
    auto draft = controller.ViewModel().viewportColliders.front().localTransform;
    draft.translation.x = 1;
    const auto edits = EvaluateColliderGroupTransform(controller.ViewModel().viewportColliders, draft, false);
    REQUIRE(edits.HasValue());
    EditorWorkspaceViewCommandData commit;
    commit.command = EditorWorkspaceViewCommand::CommitPhysicsColliders;
    commit.colliderPayload =
        ColliderViewportRequest{controller.ViewModel().physicsContext, controller.ViewModel().colliderSelectionRevision,
                                controller.ViewModel().selectedColliders, edits.Value()};
    std::size_t events = 0;
    auto subscription = controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&](const auto &) {
        ++events;
    });
    controller.ProcessCommand(commit);
    REQUIRE(controller.ViewModel().physicsEditError.empty());
    CHECK(controller.ViewModel().documentRevision.value == before.value + 1);
    CHECK(events == 1);
    const auto &colliders = controller.ViewModel().objects.front().components.colliders;
    CHECK(Math::NearlyEqual(colliders[0].localPose.translation.x, 1));
    CHECK(Math::NearlyEqual(colliders[1].localPose.translation.x, 3));
    CHECK(colliders[0].generation == 2);
    CHECK(colliders[1].generation == 2);
    controller.ProcessCommand(commit);
    CHECK_FALSE(controller.ViewModel().physicsEditError.empty());
    CHECK(events == 1);
    EditorWorkspaceViewCommandData replay;
    replay.command = EditorWorkspaceViewCommand::UndoScene;
    controller.ProcessCommand(replay);
    CHECK(events == 2);
    CHECK(controller.ViewModel().objects.front().components.colliders[0].localPose.translation.x == 0);
    CHECK(controller.ViewModel().objects.front().components.colliders[1].localPose.translation.x == 2);
}

TEST_CASE("Collider viewport selection and permission fences reject detached stale gestures", "[unit][editor][physics][collider]") {
    Runtime::RuntimeSceneService runtime;
    EditorWorkspaceController controller{"test-project", runtime, {}, {.documentSessionOwner = Identity(1)}};
    const auto id = controller.ViewModel().objects.front().id;
    Select(controller, id);
    controller.ProcessCommand(Creation(controller.ViewModel()));
    REQUIRE(controller.ViewModel().physicsEditError.empty());
    const auto &view = controller.ViewModel();
    const auto target = view.viewportColliders.front().target;
    EditorWorkspaceViewCommandData selection;
    selection.command = EditorWorkspaceViewCommand::SelectPhysicsColliders;
    selection.colliderPayload = ColliderViewportRequest{view.physicsContext, view.colliderSelectionRevision, {target}, {}};
    controller.ProcessCommand(selection);
    auto transform = controller.ViewModel().viewportColliders.front().localTransform;
    transform.translation.x = 1;
    EditorWorkspaceViewCommandData commit;
    commit.command = EditorWorkspaceViewCommand::CommitPhysicsColliders;
    commit.colliderPayload =
        ColliderViewportRequest{view.physicsContext, view.colliderSelectionRevision, {target}, {{target, 1, transform}}};
    const auto revision = view.documentRevision;
    SECTION("new child selection invalidates an earlier gesture") {
        selection.colliderPayload->selectionRevision = view.colliderSelectionRevision;
        controller.ProcessCommand(selection);
    }
    SECTION("changing scene selection cancels child mode") {
        Select(controller, id);
    }
    SECTION("a forged edit cannot target an unselected child") {
        commit.colliderPayload->edits.front().target.component = {999};
    }
    SECTION("permission is explicit") {
        commit.colliderPayload->context.canEdit = false;
    }
    SECTION("cancel wins") {
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        commit.colliderPayload->context.cancellation = cancellation.Token();
    }
    controller.ProcessCommand(commit);
    CHECK_FALSE(view.physicsEditError.empty());
    CHECK(view.documentRevision == revision);
    CHECK(view.objects.front().components.colliders.front().localPose.translation.x == 0);
}
