#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "OwnedTestDirectory.h"
#include "ProjectReplicationTestSupport.h"
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

    /** @brief Reports a typed failure for a live clone entity through the workspace's normal Play host port. */
    class DecisionNavigationTestHost final : public EditorPlaySceneHost {
    public:
        DecisionNavigationTestHost(Runtime::SceneRuntimeId scene, SceneObjectId object, AI::AgentId agent)
            : scenes_(scene), object_(object), agent_(agent) {}

        Runtime::RuntimeSceneService &Scenes() noexcept override {
            return scenes_;
        }

        Result<void> FixedUpdate(Gameplay::FixedDeltaTime) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            scenes_.Shutdown();
        }

        std::optional<EditorPlayDecisionProblem> ReadDecisionProblem() const override {
            const auto scene = scenes_.ActiveScene();
            if (!scene)
                return std::nullopt;
            return EditorPlayDecisionProblem{scene->RuntimeId(),
                                             2,
                                             agent_,
                                             EditorPlayDecisionPhase::Evaluate,
                                             MakeError(Gameplay::GameplayErrors::PhysicsUnavailable),
                                             scene->Find(Runtime::SceneObjectId{object_.value})};
        }

    private:
        Runtime::RuntimeSceneService scenes_;
        SceneObjectId object_;
        AI::AgentId agent_;
    };

    /** @brief Verify Partial Delete Undo Redo. */
    void VerifyPartialDeleteUndoRedo(TestWorkspaceController &controller, const std::vector<SceneObject> &before,
                                     std::vector<NotificationEvent> &notifications, const EditorWorkspaceViewCommandData &undo) {
        EditorWorkspaceViewCommandData partialRemove;
        partialRemove.command = EditorWorkspaceViewCommand::DeleteSelectedObjects;
        partialRemove.objectSelection = ObjectSelectionRequest{
            .objects = {before[0].id, before[1].id, SceneObjectId{999999}},
            .primary = before[1].id,
        };
        controller.ProcessCommand(partialRemove);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((notifications.size() == 2));
        REQUIRE((notifications.back().severity == NotificationSeverity::Warning));
        REQUIRE((notifications.back().message.find("2") != std::string::npos));
        REQUIRE((notifications.back().message.find("1") != std::string::npos));

        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.size() == 2));

        auto redo = SceneCommand(EditorWorkspaceViewCommand::RedoScene);
        controller.ProcessCommand(redo);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((notifications.size() == 2));
    }

    TEST_CASE("Scene Commands Publish Committed Events And Drive Undo Redo State", "[unit][editor]") {
        TestWorkspaceController controller;
        std::vector<SceneDocumentChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&events](const SceneDocumentChangedEvent &event) {
            events.push_back(event);
        });
        REQUIRE((controller.ViewModel().objects.size() == 1));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((!controller.ViewModel().canUndo));

        EditorWorkspaceViewCommandData add;
        add.command = EditorWorkspaceViewCommand::CreatePrimitive;
        add.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        controller.ProcessCommand(add);
        REQUIRE((controller.ViewModel().objects.size() == 2));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().canUndo));
        REQUIRE((events.size() == 1 && events.back().kind == DocumentChangeKind::Created));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.size() == 1));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().canRedo));
        REQUIRE((events.size() == 2 && events.back().kind == DocumentChangeKind::Undone));

        auto redo = SceneCommand(EditorWorkspaceViewCommand::RedoScene);
        controller.ProcessCommand(redo);
        REQUIRE((controller.ViewModel().objects.size() == 2));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((events.size() == 3 && events.back().kind == DocumentChangeKind::Redone));
    }

    TEST_CASE("Editor Visibility And Lock Commands Project State And Block Locked Edits", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObject original = controller.ViewModel().objects.front();
        std::vector<NotificationEvent> notifications;
        const auto subscription = controller.DataBus().Subscribe<NotificationEvent>([&notifications](const NotificationEvent &event) {
            notifications.push_back(event);
        });
        static_cast<void>(subscription);

        EditorWorkspaceViewCommandData hide;
        hide.command = EditorWorkspaceViewCommand::UpdateObjectEditorState;
        hide.objectPayload = original.id;
        hide.editorStatePayload = SceneObjectEditorState{.visible = false, .locked = false};
        controller.ProcessCommand(hide);
        REQUIRE_FALSE((controller.ViewModel().objects.front().editorState.visible));
        REQUIRE_FALSE((controller.ViewModel().objects.front().effectivelyVisible));
        REQUIRE((notifications.empty()));

        controller.ProcessCommand(EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::UndoScene});
        REQUIRE((controller.ViewModel().objects.front().editorState == SceneObjectEditorState{}));

        EditorWorkspaceViewCommandData lock;
        lock.command = EditorWorkspaceViewCommand::UpdateObjectEditorState;
        lock.objectPayload = original.id;
        lock.editorStatePayload = SceneObjectEditorState{.visible = true, .locked = true};
        controller.ProcessCommand(lock);
        REQUIRE((controller.ViewModel().objects.front().effectivelyLocked));
        REQUIRE((notifications.empty()));

        EditorWorkspaceViewCommandData rename;
        rename.command = EditorWorkspaceViewCommand::UpdateObjectName;
        rename.objectPayload = original.id;
        rename.stringPayload = "Blocked Rename";
        controller.ProcessCommand(rename);
        REQUIRE((controller.ViewModel().objects.front().name == original.name));
        REQUIRE((notifications.size() == 1));
        REQUIRE((notifications.front().severity == NotificationSeverity::Warning));

        lock.editorStatePayload = SceneObjectEditorState{};
        controller.ProcessCommand(lock);
        REQUIRE_FALSE((controller.ViewModel().objects.front().effectivelyLocked));
        REQUIRE((notifications.size() == 1));
    }

    TEST_CASE("Catalog Creation Selects The Result And Honors The Requested Parent", "[unit][editor]") {
        TestWorkspaceController controller;
        EditorWorkspaceViewCommandData createRoot;
        createRoot.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createRoot.primitivePayload = Runtime::PrimitiveId{"primitive.object.empty"};
        controller.ProcessCommand(createRoot);
        const SceneObject root = controller.ViewModel().objects.back();
        REQUIRE((root.kind == SceneObjectKind::GameObject));
        REQUIRE((!root.parent.has_value()));
        REQUIRE((controller.ViewModel().primarySelection == root.id));

        EditorWorkspaceViewCommandData createCamera;
        createCamera.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createCamera.primitivePayload = Runtime::PrimitiveId{"primitive.object.camera"};
        createCamera.objectPayload = root.id;
        controller.ProcessCommand(createCamera);
        const SceneObject camera = controller.ViewModel().objects.back();
        REQUIRE((camera.kind == SceneObjectKind::Camera));
        REQUIRE((camera.parent == root.id));
        REQUIRE((controller.ViewModel().primarySelection == camera.id));
        REQUIRE((controller.ViewModel().hierarchyRevealObject == camera.id));
    }

    TEST_CASE("Stable Selection Drives Inspector Projection And Reconciles After Delete", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        std::vector<SelectionChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<SelectionChangedEvent>([&events](const SelectionChangedEvent &event) {
            events.push_back(event);
        });

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectPayload = object;
        controller.ProcessCommand(select);
        REQUIRE((controller.ViewModel().primarySelection == object));
        REQUIRE((controller.ViewportScene().instances.front().presentation.tintStrength > 0.0F));
        REQUIRE((events.size() == 1 && events.back().kind == SelectionChangeKind::ObjectsChanged));

        EditorWorkspaceViewCommandData remove;
        remove.command = EditorWorkspaceViewCommand::DeleteObject;
        remove.objectPayload = object;
        controller.ProcessCommand(remove);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((!controller.ViewModel().primarySelection.has_value()));
        REQUIRE((events.size() == 2 && events.back().kind == SelectionChangeKind::Cleared));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.size() == 1));
        REQUIRE((controller.ViewModel().objects.front().id == object));
        REQUIRE((!controller.ViewModel().primarySelection.has_value()));
        REQUIRE((events.size() == 2));
        static_cast<void>(subscription);
    }

    TEST_CASE("Batch Delete Uses One Selection Snapshot History Entry And Snackbar", "[unit][editor]") {
        TestWorkspaceController controller;
        auto create = BoxCreationCommand();
        controller.ProcessCommand(create);
        const std::vector<SceneObject> before = controller.ViewModel().objects;
        REQUIRE((before.size() == 2));

        std::vector<NotificationEvent> notifications;
        const auto notificationSubscription =
            controller.DataBus().Subscribe<NotificationEvent>([&notifications](const NotificationEvent &event) {
            notifications.push_back(event);
        });
        static_cast<void>(notificationSubscription);

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectSelection = ObjectSelectionRequest{.objects = {before[0].id, before[1].id}, .primary = before[1].id};
        controller.ProcessCommand(select);

        const DocumentRevision beforeDeleteRevision = controller.ViewModel().documentRevision;
        EditorWorkspaceViewCommandData remove;
        remove.command = EditorWorkspaceViewCommand::DeleteSelectedObjects;
        remove.objectSelection = ObjectSelectionRequest{
            .objects = {before[0].id, before[1].id, before[0].id},
            .primary = before[1].id,
        };
        controller.ProcessCommand(remove);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((controller.ViewModel().selectedObjects.empty()));
        REQUIRE((!controller.ViewModel().primarySelection.has_value()));
        REQUIRE((!controller.ViewModel().primarySelectionWorldBounds.has_value()));
        REQUIRE((controller.ViewModel().documentRevision.value == beforeDeleteRevision.value + 1));
        REQUIRE((notifications.size() == 1));
        REQUIRE((notifications.front().severity == NotificationSeverity::Success));
        REQUIRE((notifications.front().message.find("2") != std::string::npos));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.size() == 2));
        REQUIRE((controller.ViewModel().objects[0].id == before[0].id));
        REQUIRE((controller.ViewModel().objects[1].id == before[1].id));

        VerifyPartialDeleteUndoRedo(controller, before, notifications, undo);
    }

    TEST_CASE("Viewport Picking Uses The Authoritative Selection Model", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        std::vector<SelectionChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<SelectionChangedEvent>([&events](const SelectionChangedEvent &event) {
            events.push_back(event);
        });

        EditorWorkspaceViewCommandData hit;
        hit.command = EditorWorkspaceViewCommand::PickViewport;
        hit.viewportPickPayload = ViewportPickRequest{.normalizedX = 0.5F, .normalizedY = 0.5F, .aspect = 1.0F};
        controller.ProcessCommand(hit);
        REQUIRE((controller.ViewModel().primarySelection == object));
        REQUIRE((controller.ViewportScene().instances.front().presentation.tintStrength > 0.0F));
        REQUIRE((events.size() == 1 && events.back().kind == SelectionChangeKind::ObjectsChanged));

        EditorWorkspaceViewCommandData miss;
        miss.command = EditorWorkspaceViewCommand::PickViewport;
        miss.viewportPickPayload = ViewportPickRequest{.normalizedX = 0.0F, .normalizedY = 0.0F, .aspect = 1.0F};
        controller.ProcessCommand(miss);
        REQUIRE((!controller.ViewModel().primarySelection.has_value()));
        REQUIRE((controller.ViewportScene().instances.front().presentation.tintStrength == 0.0F));
        REQUIRE((events.size() == 2 && events.back().kind == SelectionChangeKind::Cleared));
        static_cast<void>(subscription);
    }

    TEST_CASE("Transform Commands Update The Document Projection Viewport And History", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const Math::Transform original = controller.ViewModel().objects.front().localTransform;
        std::vector<SceneDocumentChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&events](const SceneDocumentChangedEvent &event) {
            events.push_back(event);
        });

        const Math::Transform edited{
            .translation = {2.0F, 3.0F, -1.0F},
            .rotation = Math::Quaternion::FromEulerRadians({0.2F, -0.3F, 0.4F}),
            .scale = {1.5F, 2.0F, 0.5F},
        };
        EditorWorkspaceViewCommandData transform;
        transform.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        transform.objectPayload = object;
        transform.transformPayload = edited;
        controller.ProcessCommand(transform);

        REQUIRE((controller.ViewModel().objects.front().localTransform == edited));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().canUndo));
        REQUIRE((events.size() == 1));
        REQUIRE((events.back().kind == DocumentChangeKind::TransformChanged));
        REQUIRE((events.back().affectedObjects == std::vector{object}));
        const Math::Vec3 worldOrigin = Math::TransformPoint(controller.ViewportScene().instances.front().localToWorld, {});
        REQUIRE((worldOrigin == edited.translation));

        auto undo = SceneCommand(EditorWorkspaceViewCommand::UndoScene);
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.front().localTransform == original));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((events.size() == 2 && events.back().kind == DocumentChangeKind::Undone));
        static_cast<void>(subscription);
    }

    TEST_CASE("Viewport Navigation Updates Only The Editor Camera Authority", "[unit][editor]") {
        TestWorkspaceController controller;
        const EditorViewportCamera before = controller.ViewportScene().camera;
        const DocumentRevision documentRevision = controller.ViewModel().documentRevision;
        std::vector<ViewportChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<ViewportChangedEvent>([&events](const ViewportChangedEvent &event) {
            events.push_back(event);
        });

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateViewport;
        navigate.viewportNavigationPayload = EditorViewportNavigationDelta{.yawRadians = 0.2F, .moveForward = 0.5F};
        controller.ProcessCommand(navigate);

        REQUIRE((controller.CurrentViewportRevision() == ViewportRevision{1}));
        REQUIRE((controller.ViewportScene().camera.IsValid()));
        REQUIRE((controller.ViewportScene().camera.position != before.position));
        REQUIRE((controller.ViewModel().documentRevision == documentRevision));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((!controller.ViewModel().canUndo));
        REQUIRE((events.size() == 1 && events.front().kind == ViewportChangeKind::CameraMoved));
        static_cast<void>(subscription);
    }

    TEST_CASE("Viewport compass commands synchronize the camera without editing the scene", "[unit][editor]") {
        TestWorkspaceController controller;
        const DocumentRevision documentRevision = controller.ViewModel().documentRevision;
        EditorWorkspaceViewCommandData align;
        align.command = EditorWorkspaceViewCommand::AlignViewportToAxis;
        align.viewportAxisPayload = EditorViewportAxisView::NegativeY;
        controller.ProcessCommand(align);

        const EditorViewportCamera &camera = controller.ViewModel().viewportCamera;
        REQUIRE(camera.IsValid());
        REQUIRE(camera.position.y < camera.target.y);
        REQUIRE(controller.ViewportScene().camera.position == camera.position);
        REQUIRE(controller.ViewModel().documentRevision == documentRevision);
        REQUIRE_FALSE(controller.ViewModel().isDirty);
    }

    TEST_CASE("Viewport Focus Treats An Empty Selection As No Interaction", "[unit][editor]") {
        TestWorkspaceController controller;
        const EditorViewportCamera before = controller.ViewportScene().camera;
        const ViewportRevision viewportRevision = controller.CurrentViewportRevision();
        std::vector<ViewportChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<ViewportChangedEvent>([&events](const ViewportChangedEvent &event) {
            events.push_back(event);
        });

        REQUIRE_FALSE(controller.ViewModel().primarySelectionWorldBounds.has_value());
        EditorWorkspaceViewCommandData focus;
        focus.command = EditorWorkspaceViewCommand::FocusViewportSelection;
        focus.floatPayload = 1.0F;
        controller.ProcessCommand(focus);

        REQUIRE((controller.CurrentViewportRevision() == viewportRevision));
        REQUIRE((controller.ViewportScene().camera.position == before.position));
        REQUIRE((controller.ViewportScene().camera.target == before.target));
        REQUIRE(events.empty());
        static_cast<void>(subscription);
    }

    TEST_CASE("Play Decision navigation honors the exact Scene and report identities", "[unit][editor][play][ai]") {
        Tests::OwnedTestDirectory project{"workspace-decision-navigation"};
        const auto scenePath = project.Path() / "assets/scenes/main.horo";
        PrepareEmptyDefaultScene(project.Path(), scenePath);
        Tests::WriteReplicationProject(project.Path());
        const auto metadataPath = project.Path() / ".horo/project.json";
        {
            std::ifstream input{metadataPath, std::ios::binary};
            auto metadata = nlohmann::json::parse(input);
            input.close();
            metadata.at("settings")["defaultScene"] = "assets/scenes/main.horo";
            std::ofstream output{metadataPath, std::ios::binary | std::ios::trunc};
            output << metadata.dump();
            REQUIRE(output.good());
        }
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        const auto agent = AI::AgentId::Create(41).Value();
        constexpr auto original = SceneObjectId{2};
        const SceneDocumentSnapshot
            document{.revision = DocumentRevision{1},
                     .state = DocumentStateId{1},
                     .objects = {{.id = SceneObjectId{1}, .name = "Other object", .components = {.camera = Runtime::CameraComponent{}}},
                                 {.id = original, .name = "AI agent", .components = {.aiAgent = AI::AiAgentComponent{.agent = agent}}}}};
        const auto fingerprint = InspectProjectSceneFingerprint(project.Path(), scenePath);
        REQUIRE(fingerprint.HasValue());
        REQUIRE(SaveProjectScene(project.Path(), scenePath, document, fingerprint.Value(), false, mutations, files).HasValue());

        CancellationSource cancellation;
        Runtime::RuntimeSceneService preview;
        REQUIRE(preview.Startup(cancellation.Token()).HasValue());
        EditorWorkspaceDependencies dependencies;
        dependencies.gameplay.sceneFactory = [=](Runtime::SceneRuntimeId scene, const Runtime::RuntimeSceneDefinition &) {
            return Result<std::unique_ptr<EditorPlaySceneHost>>::Success(
                std::make_unique<DecisionNavigationTestHost>(scene, original, agent));
        };
        EditorWorkspaceController workspace{project.Path(), preview, {}, dependencies};
        REQUIRE_FALSE(workspace.InitializationError());

        const auto play = [&] {
            workspace.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::StartPlay));
            workspace.UpdatePlayPresentation(0.0F);
            INFO(workspace.ViewModel().playError);
            REQUIRE(workspace.ViewModel().playState == EditorPlayState::Playing);
            REQUIRE(workspace.ViewModel().playDecisionProblem);
            REQUIRE(workspace.ViewModel().playDecisionObjectAvailable);
            return *workspace.ViewModel().playDecisionProblem;
        };
        const auto first = play();
        REQUIRE(first.entity);
        const auto previousSelection = workspace.ViewModel().primarySelection;
        EditorWorkspaceViewCommandData navigate{.command = EditorWorkspaceViewCommand::NavigatePlayDecisionProblem,
                                                .playDecisionNavigation =
                                                    PlayDecisionNavigationRequest{first.tick + 1, first.agent, *first.entity}};
        workspace.ProcessCommand(navigate);
        CHECK(workspace.ViewModel().primarySelection == previousSelection);
        navigate.playDecisionNavigation->tick = first.tick;
        workspace.ProcessCommand(navigate);
        CHECK(workspace.ViewModel().primarySelection == original);
        CHECK(workspace.ViewModel().hierarchyRevealObject == original);

        workspace.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::StopPlay));
        CHECK_FALSE(workspace.ViewModel().playDecisionProblem);
        const auto second = play();
        CHECK(second.scene != first.scene);
        workspace.ProcessCommand({.command = EditorWorkspaceViewCommand::SelectObject,
                                  .objectSelection = ObjectSelectionRequest{.objects = {SceneObjectId{1}}, .primary = SceneObjectId{1}}});
        const auto beforeStaleNavigation = workspace.ViewModel().primarySelection;
        REQUIRE(beforeStaleNavigation != original);
        workspace.ProcessCommand(navigate);
        CHECK(workspace.ViewModel().primarySelection == beforeStaleNavigation);
        workspace.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::StopPlay));
    }

}  // namespace
