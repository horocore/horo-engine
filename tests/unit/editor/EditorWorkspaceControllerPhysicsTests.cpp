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
