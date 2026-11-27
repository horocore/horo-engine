#include "DecisionGraphCompilerTestSupport.h"
#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Scene/SceneSource.h"
#include "OwnedTestDirectory.h"
#include "ProjectReplicationTestSupport.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/commands/EditorInspectionCommandEndpoint.h"
#include "editor/screens/workspace/gameplay/GameplayBehaviorRequestValidation.h"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    namespace Math = Math;
    using TestWorkspaceController = HoroEditorWorkspaceControllerPolicyTests::FocusedWorkspaceController;
    using namespace HoroEditorWorkspaceControllerFilesystemTests;

    /** @brief Records owner admission while leaving native readback to renderer-specific tests. */
    class CaptureViewport final : public IEditorViewportRenderer {
    public:
        Result<std::uint64_t> RequestImageCapture(EditorViewportImageRegion region, std::function<bool()> valid) override {
            submitted = region;
            current = std::move(valid);
            ++requests;
            return Result<std::uint64_t>::Success(requests);
        }

        bool CancelImageCapture(const std::uint64_t id) noexcept override {
            cancelledId = id;
            if (onCancel)
                onCancel();
            return true;
        }

        Result<void> ExecutePreparedFrame(Render::RenderFrameScope &, std::span<const Render::RenderPassDescriptor>) override {
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        }

        Result<void> ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &) override {
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        }

        void RequestExtent(EditorViewportExtent value) noexcept override {
            extent = value;
        }

        void RequestGrid(const EditorViewportGridOptions &) noexcept override {}

        void RequestLightVisualizer(const EditorViewportLightVisualizerOptions &) noexcept override {}

        EditorViewportExtent RequestedExtent() const noexcept override {
            return extent;
        }

        Math::ClipDepthRange ClipDepthRange() const noexcept override {
            return Math::ClipDepthRange::ZeroToOne;
        }

        EditorViewportTextureView TextureView() const noexcept override {
            return {};
        }

        bool IsReady() const noexcept override {
            return extent.IsValid();
        }

        EditorViewportExtent extent{640, 480};
        EditorViewportImageRegion submitted;
        std::function<bool()> current;
        std::function<void()> onCancel;
        std::uint64_t requests{};
        std::optional<std::uint64_t> cancelledId;
    };

    /** @brief Reports a typed failure for a live clone entity through the workspace's normal Play host port. */
    class DecisionNavigationTestHost final : public EditorPlaySceneHost {
    public:
        DecisionNavigationTestHost(Runtime::SceneRuntimeId scene, SceneObjectId object, AI::AgentId agent, AI::DecisionGraphAssetId graph,
                                   AI::DecisionNodeId node)
            : scenes_(scene), object_(object), agent_(agent), graph_(graph), node_(node) {}

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
                                             scene->Find(Runtime::SceneObjectId{object_.value}),
                                             graph_,
                                             node_};
        }

        std::optional<EditorPlayContactSnapshot> ReadAgentSceneContacts(const Runtime::EntityRef object) override {
            const auto scene = scenes_.ActiveScene();
            if (!scene || scene->Get(object).HasError())
                return std::nullopt;
            auto peer = scene->Find(contactPeer).value_or(Runtime::EntityRef{}).entity;
            if (staleContactPeer)
                ++peer.generation;
            return EditorPlayContactSnapshot{.anchor = object,
                                             .structuralRevision = scene->StructuralRevision(),
                                             .completedTick = 42,
                                             .contacts = {{{peer, EditorPlayContactSnapshot::Kind::TriggerEnter},
                                                           {peer, EditorPlayContactSnapshot::Kind::TriggerExit},
                                                           {peer, EditorPlayContactSnapshot::Kind::ContactEnd}}},
                                             .count = 3};
        }

        Runtime::SceneObjectId contactPeer{1};
        bool staleContactPeer{};

    private:
        Runtime::RuntimeSceneService scenes_;
        SceneObjectId object_;
        AI::AgentId agent_;
        AI::DecisionGraphAssetId graph_;
        AI::DecisionNodeId node_;
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
        const auto revision = [&] {
            const auto fence = controller.ViewModel().assetDropFence;
            return InspectionSceneRevision{fence.replacementEpoch, fence.revision.value};
        };
        const auto endpoint = controller.Controller().BindSceneInspection("test-project", 1).lock();
        REQUIRE(endpoint);
        REQUIRE(endpoint->inspectEditTargets);
        const std::array targets{original.id.value};
        const auto initial = revision();
        CHECK(endpoint->inspectEditTargets(initial, targets).HasValue());
        const std::array absent{std::uint64_t{0}};
        CHECK(endpoint->inspectEditTargets(initial, absent).HasError());
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
        CHECK(endpoint->inspectEditTargets(revision(), targets).HasError());
        CHECK(endpoint->inspectEditTargets(initial, targets).HasError());
        REQUIRE((notifications.empty()));

        controller.ProcessCommand(EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::UndoScene});
        REQUIRE((controller.ViewModel().objects.front().editorState == SceneObjectEditorState{}));

        EditorWorkspaceViewCommandData lock;
        lock.command = EditorWorkspaceViewCommand::UpdateObjectEditorState;
        lock.objectPayload = original.id;
        lock.editorStatePayload = SceneObjectEditorState{.visible = true, .locked = true};
        controller.ProcessCommand(lock);
        REQUIRE((controller.ViewModel().objects.front().effectivelyLocked));
        CHECK(endpoint->inspectEditTargets(revision(), targets).HasError());
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
        CHECK(endpoint->inspectEditTargets(revision(), targets).HasValue());
        REQUIRE((notifications.size() == 1));
    }

    TEST_CASE("Scene proposal bindings retire on replacement or workspace shutdown", "[unit][editor][scene-review]") {
        Runtime::RuntimeSceneService scenes;
        CancellationSource process;
        EditorWorkspaceDependencies dependencies;
        dependencies.world.documentSessionOwner = Assets::AssetId::FromBytes({1});
        EditorWorkspaceController controller{"scene-binding-project", scenes, {}, dependencies};
        REQUIRE(scenes.Startup(process.Token()).HasValue());
        const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, process.Token()};
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        controller.SynchronizeRuntimeScenePreview();
        const auto original = controller.ViewModel().objects.front();
        const auto fence = controller.ViewModel().assetDropFence;
        const InspectionSceneRevision revision{fence.replacementEpoch, fence.revision.value};
        const auto endpoint = controller.BindSceneInspection("test-project", 1).lock();
        REQUIRE(endpoint);
        const std::array targets{original.id.value};
        const ObjectInspectionRequest query{.objects = {original.id.value}, .expected = revision};
        const std::array renames{AgentSceneObjectRename{original.id.value, "Reviewed name"}};
        const auto allowed = [] { return true; };
        REQUIRE(endpoint->readTransforms(query).HasValue());
        REQUIRE(endpoint->inspectEditTargets(revision, targets).HasValue());
        std::shared_ptr<const EditorInspectionCommandEndpoint> replacement;

        SECTION("a proposal already on screen loses its old binding") {
            const auto review = endpoint->proposeEdits(revision, {}, renames, {}, allowed);
            REQUIRE(review.HasValue());
            replacement = controller.BindSceneInspection("test-project", 2).lock();
            REQUIRE(replacement);
            CHECK(controller.ApproveAgentSceneReview(review.Value()).HasError());
            CHECK_FALSE(controller.ViewModel().agentSceneReview);
        }
        SECTION("replacement during admission cannot publish a proposal from the old binding") {
            const auto review = endpoint->proposeEdits(revision, {}, renames, {}, [&] {
                replacement = controller.BindSceneInspection("test-project", 2).lock();
                return true;
            });
            REQUIRE(replacement);
            CHECK(review.HasError());
            CHECK_FALSE(controller.ViewModel().agentSceneReview);
        }
        SECTION("workspace shutdown retires the retained callbacks") {
            controller.RevokeWorkspaceCommands();
        }

        CHECK(endpoint->readTransforms(query).HasError());
        CHECK(endpoint->inspectEditTargets(revision, targets).HasError());
        CHECK(endpoint->proposeEdits(revision, {}, renames, {}, allowed).HasError());
        CHECK(controller.ViewModel().objects.front().name == original.name);
        CHECK_FALSE(controller.ViewModel().isDirty);
        CHECK_FALSE(controller.ViewModel().canUndo);
        if (replacement) {
            CHECK(replacement->readTransforms(query).HasValue());
            CHECK(replacement->inspectEditTargets(revision, targets).HasValue());
            const auto fresh = replacement->proposeEdits(revision, {}, renames, {}, allowed);
            REQUIRE(fresh.HasValue());
            REQUIRE(controller.ApproveAgentSceneReview(fresh.Value()).HasValue());
            CHECK(controller.ViewModel().objects.front().name == renames.front().name);
            CHECK(controller.ViewModel().isDirty);
            controller.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::UndoScene));
            CHECK(controller.ViewModel().objects.front().name == original.name);
            CHECK_FALSE(controller.ViewModel().canUndo);
        }
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
        const auto evidence = controller.Controller().CaptureAgentSceneSelection();
        REQUIRE(evidence);
        REQUIRE(evidence->objectCount == 1);
        CHECK(evidence->objects.front() == object.value);
        CHECK_FALSE(evidence->lifetime.IsCancellationRequested());

        EditorWorkspaceViewCommandData remove;
        remove.command = EditorWorkspaceViewCommand::DeleteObject;
        remove.objectPayload = object;
        controller.ProcessCommand(remove);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((!controller.ViewModel().primarySelection.has_value()));
        REQUIRE((events.size() == 2 && events.back().kind == SelectionChangeKind::Cleared));
        CHECK(evidence->lifetime.IsCancellationRequested());
        CHECK_FALSE(controller.Controller().CaptureAgentSceneSelection());

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

    TEST_CASE("Authored viewport shows a separate agent proposal ghost until local review is resolved", "[unit][editor][scene-review]") {
        CaptureViewport viewport;
        Runtime::RuntimeSceneService scenes;
        CancellationSource process;
        EditorWorkspaceDependencies dependencies;
        dependencies.world.documentSessionOwner = Assets::AssetId::FromBytes({1});
        EditorWorkspaceController controller{"scene-review-project", scenes, {}, dependencies};
        REQUIRE(scenes.Startup(process.Token()).HasValue());
        const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, process.Token()};
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        controller.SynchronizeRuntimeScenePreview();

        REQUIRE(controller.ViewModel().objects.size() == 1);
        const auto object = controller.ViewModel().objects.front();
        const auto fence = controller.ViewModel().assetDropFence;
        const InspectionSceneRevision revision{fence.replacementEpoch, fence.revision.value};
        const auto origin = [&] {
            return Math::TransformPoint(controller.ViewportScene().instances.front().localToWorld, {});
        };
        REQUIRE(controller.ViewportScene().instances.size() == 1);
        const auto baseline = origin();
        auto desired = object.localTransform;
        desired.translation.x += 3.0F;
        const std::array updates{SceneObjectTransformUpdate{object.id, desired}};
        const std::array renames{AgentSceneObjectRename{object.id.value, "Reviewed name"}};
        std::vector<SceneDocumentChangedEvent> events;
        const auto subscription = controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&](const auto &event) {
            events.push_back(event);
        });
        unsigned withdrawn{};
        const auto notices = controller.DataBus().Subscribe<NotificationEvent>([&](const NotificationEvent &event) {
            if (event.deduplicationKey == "scene::agent_scene_review_withdrawn") {
                CHECK(event.severity == NotificationSeverity::Warning);
                ++withdrawn;
            }
        });
        CancellationSource proposal;
        const std::array duplicate{renames.front(), renames.front()};
        CHECK(controller.PreviewAgentSceneEdits(revision, updates, duplicate, proposal.Token()).HasError());
        CHECK_FALSE(controller.ViewModel().agentSceneReview);
        const auto denied = controller.PreviewAgentSceneEdits(revision, updates, renames, proposal.Token(), []() -> bool {
            throw std::runtime_error("fixture authorization failure");
        });
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == SceneTransactionError(SceneTransactionFailure::Unauthorized).code.Value());
        CHECK_FALSE(controller.ViewModel().agentSceneReview);
        CHECK(origin() == baseline);

        CancellationSource abandoned;
        abandoned.RequestCancellation();
        const auto cancelled = controller.PreviewAgentSceneEdits(revision, updates, renames, abandoned.Token(), [] {
            FAIL_CHECK("Cancelled proposals must not ask for authorization");
            return true;
        });
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == SceneTransactionError(SceneTransactionFailure::Cancelled).code.Value());
        CHECK_FALSE(controller.ViewModel().agentSceneReview);
        std::optional<std::uint64_t> replaceOnAuthorityCheck;
        std::optional<std::uint64_t> replacementReview;
        std::function<void()> afterReviewRetirement;
        bool requireVisibleReview{};
        unsigned authorizationChecks{};
        const auto sceneRevisionBeforeReview = controller.CurrentViewportSceneRevision();
        const auto preview = controller.PreviewAgentSceneEdits(revision, updates, renames, proposal.Token(), [&] {
            ++authorizationChecks;
            if (const auto previous = std::exchange(replaceOnAuthorityCheck, std::nullopt)) {
                REQUIRE(controller.CancelAgentSceneReview(*previous));
                const std::array replacement{AgentSceneObjectRename{object.id.value, "Replacement review"}};
                const auto shown = controller.PreviewAgentSceneEdits(revision, {}, replacement, proposal.Token());
                REQUIRE(shown.HasValue());
                replacementReview = shown.Value();
            }
            if (!controller.ViewModel().agentSceneReview)
                if (auto retired = std::exchange(afterReviewRetirement, {}))
                    retired();
            return !requireVisibleReview || controller.ViewModel().agentSceneReview.has_value();
        });
        REQUIRE(preview.HasValue());
        REQUIRE(controller.ViewModel().agentSceneReview);
        CHECK(controller.ViewModel().agentSceneReview->objectCount == 1);
        REQUIRE(controller.ViewModel().agentSceneReview->transforms.size() == 1);
        const auto &reviewedTransform = controller.ViewModel().agentSceneReview->transforms.front();
        CHECK(reviewedTransform.object == object.id.value);
        CHECK(reviewedTransform.before == object.localTransform);
        CHECK(reviewedTransform.after == desired);
        REQUIRE(controller.ViewModel().agentSceneReview->renames.size() == 1);
        CHECK(controller.ViewModel().agentSceneReview->renames.front().before == object.name);
        CHECK(controller.ViewModel().agentSceneReview->renames.front().after == renames.front().name);
        REQUIRE(controller.ViewModel().agentSceneReview->hasSpatialPreview);
        const auto reviewSceneRevision = controller.CurrentViewportSceneRevision();
        CHECK(reviewSceneRevision != sceneRevisionBeforeReview);
        const auto &geometry = controller.ViewportScene();
        REQUIRE(geometry.instances.size() == 2);
        CHECK(origin() == baseline);
        CHECK(Math::TransformPoint(geometry.instances.back().localToWorld, {}) == desired.translation);
        CHECK(geometry.instances.back().presentation.tintStrength > 0.0F);
        CHECK_FALSE(geometry.instanceObjects.back().IsValid());
        CHECK(geometry.instancePickable.back() == 0);
        CHECK(geometry.instanceMotionObjects.size() == 1);
        CHECK(geometry.transformOverrides.front().present == false);
        CHECK(controller.ViewModel().objects.front().localTransform == object.localTransform);
        CHECK(controller.ViewModel().objects.front().name == object.name);
        CHECK_FALSE(controller.ViewModel().isDirty);
        CHECK(events.empty());
        CHECK(controller.PreviewAgentSceneEdits(revision, updates, renames, proposal.Token()).HasError());

        SECTION("approval uses canonical history and consumes the review") {
            using Action = EditorWorkspaceViewCommandData::AgentSceneReviewDecision::Action;
            CHECK(controller.ViewModel().agentSceneReview->requiresSpatialWarning);
            CHECK_FALSE(controller.ViewModel().agentSceneReview->spatialImpactConfirmed);
            const auto premature = controller.ApproveAgentSceneReview(preview.Value());
            REQUIRE(premature.HasError());
            CHECK(premature.ErrorValue().code.Value() == SceneTransactionError(SceneTransactionFailure::Unauthorized).code.Value());
            CHECK(controller.ConfirmAgentSceneReviewImpact(preview.Value() + 1).HasError());
            EditorWorkspaceViewCommandData approval;
            approval.command = EditorWorkspaceViewCommand::ResolveAgentSceneReview;
            approval.agentSceneReviewDecision = {preview.Value(), Action::ConfirmSpatialImpact};
            controller.ProcessCommand(approval);
            REQUIRE(controller.ViewModel().agentSceneReview);
            CHECK(controller.ViewModel().agentSceneReview->spatialImpactConfirmed);
            CHECK(events.empty());
            CHECK_FALSE(controller.ViewModel().isDirty);
            CHECK_FALSE(controller.ViewModel().canUndo);
            approval.agentSceneReviewDecision = {preview.Value(), Action::Apply};
            controller.ProcessCommand(approval);
            CHECK(controller.ViewModel().objects.front().localTransform == desired);
            CHECK(controller.ViewModel().objects.front().name == renames.front().name);
            CHECK_FALSE(controller.ViewModel().agentSceneReview);
            REQUIRE(events.size() == 1);
            CHECK(events.front().affectedObjects == std::vector{object.id});
            CHECK(controller.ApproveAgentSceneReview(preview.Value()).HasError());
            controller.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::UndoScene));
            CHECK(controller.ViewModel().objects.front().localTransform == object.localTransform);
            CHECK(controller.ViewModel().objects.front().name == object.name);
            CHECK_FALSE(controller.ViewModel().isDirty);
        }
        SECTION("cancellation removes the overlay without a document event") {
            CHECK_FALSE(controller.CancelAgentSceneReview(preview.Value() + 1));
            CHECK(controller.CancelAgentSceneReview(preview.Value()));
            CHECK(origin() == baseline);
            CHECK(controller.ViewportScene().instances.size() == 1);
            CHECK(controller.CurrentViewportSceneRevision() != reviewSceneRevision);
            CHECK_FALSE(controller.ViewModel().canUndo);
            CHECK(events.empty());
            CHECK(withdrawn == 0);
        }
        SECTION("authored image capture excludes proposed geometry") {
            const auto endpoint = controller.BindDebugCapture("scene-review-project", 1, &viewport).lock();
            REQUIRE(endpoint);
            CHECK_FALSE(endpoint->readImageSource());

            REQUIRE(controller.CancelAgentSceneReview(preview.Value()));
            const auto before = endpoint->readImageSource();
            REQUIRE(before);
            constexpr EditorViewportImageRegion region{2, 3, 24, 16};
            const auto allowed = [] { return true; };
            const auto capture = endpoint->startImage(*before, region, allowed);
            REQUIRE(capture.HasValue());

            const auto reopened = controller.PreviewAgentSceneEdits(revision, updates, renames, proposal.Token());
            REQUIRE(reopened.HasValue());
            CHECK(viewport.cancelledId == capture.Value());
            CHECK_FALSE(viewport.current());
            CHECK_FALSE(endpoint->readImageSource());
            CHECK(endpoint->startImage(*before, region, allowed).HasError());
            auto proposedSource = *before;
            proposedSource.projectionRevision = controller.CurrentViewportSceneRevision();
            CHECK(endpoint->startImage(proposedSource, region, allowed).HasError());
            CHECK(viewport.requests == 1);

            REQUIRE(controller.CancelAgentSceneReview(reopened.Value()));
            const auto after = endpoint->readImageSource();
            REQUIRE(after);
            CHECK(*after != *before);
            CHECK(endpoint->startImage(*after, region, allowed).HasValue());
            CHECK(events.empty());
            CHECK_FALSE(controller.ViewModel().canUndo);
        }
        SECTION("approval cannot consume a different proposal installed during the authority check") {
            REQUIRE(controller.ConfirmAgentSceneReviewImpact(preview.Value()).HasValue());
            replaceOnAuthorityCheck = preview.Value();
            CHECK(controller.ApproveAgentSceneReview(preview.Value()).HasError());
            REQUIRE(replacementReview);
            REQUIRE(controller.ViewModel().agentSceneReview);
            CHECK(controller.ViewModel().agentSceneReview->id == *replacementReview);
            CHECK_FALSE(controller.ViewModel().agentSceneReview->spatialImpactConfirmed);
            CHECK(controller.ViewModel().objects.front().name == object.name);
            CHECK(controller.ViewModel().objects.front().localTransform == object.localTransform);
            CHECK_FALSE(controller.ViewModel().isDirty);
            CHECK_FALSE(controller.ViewModel().canUndo);
            CHECK(events.empty());
        }
        SECTION("approval rechecks authority after retiring the viewport review") {
            REQUIRE(controller.ConfirmAgentSceneReviewImpact(preview.Value()).HasValue());
            requireVisibleReview = true;
            const auto deniedApproval = controller.ApproveAgentSceneReview(preview.Value());
            REQUIRE(deniedApproval.HasError());
            CHECK(deniedApproval.ErrorValue().code.Value() == SceneTransactionError(SceneTransactionFailure::Unauthorized).code.Value());
            CHECK_FALSE(controller.ViewModel().agentSceneReview);
            CHECK(controller.ViewportScene().instances.size() == 1);
            CHECK(origin() == baseline);
            CHECK(controller.ViewModel().objects.front().name == object.name);
            CHECK_FALSE(controller.ViewModel().isDirty);
            CHECK_FALSE(controller.ViewModel().canUndo);
            CHECK(events.empty());
        }
        SECTION("a replacement published during the final authority check supersedes the approval") {
            REQUIRE(controller.ConfirmAgentSceneReviewImpact(preview.Value()).HasValue());
            afterReviewRetirement = [&] {
                const std::array replacement{AgentSceneObjectRename{object.id.value, "New review"}};
                const auto staged = controller.PreviewAgentSceneEdits(revision, {}, replacement, proposal.Token());
                REQUIRE(staged.HasValue());
                replacementReview = staged.Value();
            };
            const auto stale = controller.ApproveAgentSceneReview(preview.Value());
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == SceneTransactionError(SceneTransactionFailure::Stale).code.Value());
            REQUIRE(replacementReview);
            REQUIRE(controller.ViewModel().agentSceneReview);
            CHECK(controller.ViewModel().agentSceneReview->id == *replacementReview);
            CHECK(controller.ViewModel().objects.front().name == object.name);
            CHECK(origin() == baseline);
            CHECK_FALSE(controller.ViewModel().canUndo);
            CHECK(events.empty());
        }
        SECTION("viewport capture cancellation can withdraw a proposal before staging returns") {
            REQUIRE(controller.CancelAgentSceneReview(preview.Value()));
            const auto endpoint = controller.BindDebugCapture("scene-review-project", 1, &viewport).lock();
            REQUIRE(endpoint);
            const auto source = endpoint->readImageSource();
            REQUIRE(source);
            const auto capture = endpoint->startImage(*source, {2, 3, 24, 16}, [] { return true; });
            REQUIRE(capture.HasValue());
            viewport.onCancel = [&proposal] { proposal.RequestCancellation(); };

            const auto denied = controller.PreviewAgentSceneEdits(revision, updates, renames, proposal.Token());
            REQUIRE(denied.HasError());
            CHECK(denied.ErrorValue().code.Value() == SceneTransactionError(SceneTransactionFailure::Cancelled).code.Value());
            CHECK(viewport.cancelledId == capture.Value());
            CHECK_FALSE(controller.ViewModel().agentSceneReview);
            CHECK(controller.ViewportScene().instances.size() == 1);
            CHECK(origin() == baseline);
            CHECK_FALSE(controller.ViewModel().canUndo);
            CHECK(events.empty());
        }
        SECTION("closing command authority retires the overlay on the owner tick") {
            controller.RevokeWorkspaceCommands();
            controller.UpdatePlayPresentation(0.0F);
            controller.UpdatePlayPresentation(0.0F);
            CHECK(origin() == baseline);
            CHECK(controller.ViewportScene().instances.size() == 1);
            CHECK(controller.ApproveAgentSceneReview(preview.Value()).HasError());
            CHECK(events.empty());
            CHECK(withdrawn == 1);
        }
        SECTION("approval of a cancelled proposal reports withdrawal once") {
            REQUIRE(controller.ConfirmAgentSceneReviewImpact(preview.Value()).HasValue());
            proposal.RequestCancellation();
            const auto previousChecks = authorizationChecks;
            EditorWorkspaceViewCommandData approval;
            approval.command = EditorWorkspaceViewCommand::ResolveAgentSceneReview;
            approval.agentSceneReviewDecision = {preview.Value(), EditorWorkspaceViewCommandData::AgentSceneReviewDecision::Action::Apply};
            controller.ProcessCommand(approval);
            CHECK(origin() == baseline);
            CHECK(withdrawn == 1);
            CHECK(authorizationChecks == previousChecks);
            CHECK(events.empty());
        }
        SECTION("a lost runtime scene cannot leave the retired preview visible") {
            scenes.Shutdown();
            controller.SynchronizeRuntimeScenePreview();
            CHECK_FALSE(controller.ViewModel().agentSceneReview);
            CHECK(controller.ViewportScene().instances.empty());
            CHECK(controller.ViewportScene().transformOverrides.empty());
            CHECK(controller.ApproveAgentSceneReview(preview.Value()).HasError());
            CHECK(events.empty());
            CHECK_FALSE(controller.ViewModel().isDirty);
            CHECK(withdrawn == 1);
        }
    }

    TEST_CASE("Approved agent edits checkpoint their exact Scene state on the next enabled autosave tick", "[unit][editor][scene-review]") {
        Tests::OwnedTestDirectory project{"workspace-agent-recovery"};
        const auto root = std::filesystem::canonical(project.Path());
        const auto scenePath = root / "assets/scenes/main.horo";
        PrepareEmptyDefaultScene(root, scenePath);
        const SceneSource::SceneObjectSnapshot source{.id = {7}, .name = "Original"};
        const auto encoded = SceneSource::EncodeSceneSource({.objects = std::span{&source, 1}});
        REQUIRE(static_cast<bool>(std::ofstream{scenePath, std::ios::binary} << encoded));

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        Runtime::RuntimeSceneService scenes;
        CancellationSource process;
        EditorWorkspaceDependencies dependencies;
        dependencies.world.documentSessionOwner = Assets::AssetId::FromBytes({1});
        dependencies.mutations = &mutations;
        dependencies.durableFiles = &files;
        EditorWorkspaceController controller{root, scenes, {}, dependencies};
        REQUIRE(scenes.Startup(process.Token()).HasValue());
        const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, process.Token()};
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        controller.SynchronizeRuntimeScenePreview();

        const auto fence = controller.ViewModel().assetDropFence;
        const std::array rename{AgentSceneObjectRename{source.id.value, "Approved name"}};
        const auto preview = controller.PreviewAgentSceneEdits({fence.replacementEpoch, fence.revision.value}, {}, rename, {});
        REQUIRE(preview.HasValue());
        controller.UpdateAutosave(1.0F, 10);
        REQUIRE_FALSE(InspectProjectSceneRecovery(root, scenePath).Value());
        REQUIRE(controller.ApproveAgentSceneReview(preview.Value()).HasValue());
        REQUIRE_FALSE(InspectProjectSceneRecovery(root, scenePath).Value());

        SECTION("enabled autosave checkpoints the approved edit without saving the Scene") {
            controller.UpdateAutosave(1.0F, 10);
            const auto recovery = InspectProjectSceneRecovery(root, scenePath);
            REQUIRE(recovery.HasValue());
            REQUIRE(recovery.Value());
            REQUIRE(recovery.Value()->objects.size() == 1);
            CHECK(recovery.Value()->objects.front().name == rename.front().name);
            REQUIRE(recovery.Value()->approval);
            CHECK(recovery.Value()->approval->reviewId == preview.Value());
            CHECK(recovery.Value()->approval->baseRevision.value == fence.revision.value);
            CHECK(recovery.Value()->approval->affectedObjects == 1);
            const auto saved = LoadProjectScene(root, scenePath);
            REQUIRE(saved.HasValue());
            REQUIRE(saved.Value().objects.size() == 1);
            CHECK(saved.Value().objects.front().name == source.name);
        }
        SECTION("undo retires the approved snapshot before it can write recovery") {
            controller.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::UndoScene));
            controller.UpdateAutosave(1.0F, 10);
            CHECK_FALSE(controller.ViewModel().isDirty);
            CHECK_FALSE(InspectProjectSceneRecovery(root, scenePath).Value());
        }
        SECTION("disabled autosave leaves the approved edit in memory") {
            controller.UpdateAutosave(1.0F, 0);
            CHECK(controller.ViewModel().isDirty);
            CHECK_FALSE(InspectProjectSceneRecovery(root, scenePath).Value());
        }
    }

    TEST_CASE("A full viewport draw budget preserves committed geometry for text-only review", "[unit][editor][scene-review]") {
        Runtime::RuntimeSceneService scenes;
        CancellationSource process;
        EditorWorkspaceDependencies dependencies;
        dependencies.world.documentSessionOwner = Assets::AssetId::FromBytes({1});
        EditorWorkspaceController controller{"scene-review-capacity", scenes, {}, dependencies};
        REQUIRE(scenes.Startup(process.Token()).HasValue());
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 1, .cancellation = process.Token()})
                    .HasValue());
        controller.SynchronizeRuntimeScenePreview();
        const auto scene = scenes.ActiveScene();
        REQUIRE(scene);
        auto snapshot = controller.ViewportScene();
        REQUIRE(snapshot.instances.size() == 1);
        const auto mesh = snapshot.instances.front();
        const auto object = snapshot.instanceObjects.front();
        const auto entity = snapshot.instanceEntities.front();
        constexpr auto Capacity = Render::RenderGraphMaterialRaster::MaximumDraws;
        snapshot.instances.resize(Capacity, mesh);
        snapshot.instanceObjects.resize(Capacity, object);
        snapshot.instanceEntities.resize(Capacity, entity);
        snapshot.instancePickable.resize(Capacity, 1);
        snapshot.stagedInstanceWorlds.resize(Capacity);
        const std::array previews{SceneObjectTransformPreview{object, Math::Transform{.translation = {10, 0, 0}}}};
        const auto result = AppendEditorViewportSceneReviewGhosts(*scene, previews, snapshot);
        REQUIRE(result.HasValue());
        CHECK_FALSE(result.Value());
        CHECK(snapshot.instances.size() == Capacity);
        CHECK(snapshot.instances.back().localToWorld == mesh.localToWorld);
        CHECK(snapshot.instanceObjects.back() == object);
        CHECK(snapshot.instancePickable.back() == 1);
    }

    TEST_CASE("Agent Scene review applies light and object edits in one undo entry", "[unit][editor][scene-review]") {
        Runtime::RuntimeSceneService scenes;
        CancellationSource process;
        EditorWorkspaceDependencies dependencies;
        dependencies.world.documentSessionOwner = Assets::AssetId::FromBytes({1});
        EditorWorkspaceController controller{"light-review-project", scenes, {}, dependencies};
        REQUIRE(scenes.Startup(process.Token()).HasValue());
        const Runtime::FrameContext initial{1, {}, 0.0, 0, {}, false, process.Token()};
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, initial).HasValue());
        controller.SynchronizeRuntimeScenePreview();

        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrimitive;
        create.primitivePayload = Runtime::PrimitiveId{"primitive.object.light_point"};
        controller.ProcessCommand(create);
        const Runtime::FrameContext next{2, {}, 0.0, 0, {}, false, process.Token()};
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, next).HasValue());
        controller.SynchronizeRuntimeScenePreview();
        REQUIRE(controller.ViewModel().objects.size() == 2);
        const auto object = controller.ViewModel().objects.front();
        const auto light = controller.ViewModel().objects.back();
        REQUIRE(light.components.light);
        REQUIRE(light.components.light->enabled);

        const auto fence = controller.ViewModel().assetDropFence;
        const InspectionSceneRevision revision{fence.replacementEpoch, fence.revision.value};
        const std::array renames{AgentSceneObjectRename{object.id.value, "Reviewed box"}};
        const std::array lights{AgentSceneLightUpdate{light.id.value, false}};
        const std::array invalid{AgentSceneLightUpdate{object.id.value, false}};
        CHECK(controller.PreviewAgentSceneEdits(revision, {}, renames, {}, {}, invalid).HasError());
        CHECK_FALSE(controller.ViewModel().agentSceneReview);
        CHECK(controller.ViewModel().objects.front().name == object.name);

        const auto preview = controller.PreviewAgentSceneEdits(revision, {}, renames, {}, {}, lights);
        REQUIRE(preview.HasValue());
        REQUIRE(controller.ViewModel().agentSceneReview);
        const auto &review = *controller.ViewModel().agentSceneReview;
        CHECK(review.objectCount == 2);
        REQUIRE(review.lights.size() == 1);
        CHECK(review.lights.front().object == light.id.value);
        CHECK_FALSE(review.lights.front().enabled);
        CHECK(controller.ViewModel().objects.front().name == object.name);
        CHECK(controller.ViewModel().objects.back().components.light->enabled);
        CHECK(controller.ViewModel().documentRevision == fence.revision);

        REQUIRE(controller.ApproveAgentSceneReview(preview.Value()).HasValue());
        CHECK(controller.ViewModel().objects.front().name == renames.front().name);
        CHECK_FALSE(controller.ViewModel().objects.back().components.light->enabled);
        controller.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::UndoScene));
        CHECK(controller.ViewModel().objects.front().name == object.name);
        CHECK(controller.ViewModel().objects.back().components.light->enabled);
    }

    TEST_CASE("Scene inspection resolves unordered pages and validates objects outside the returned range", "[unit][editor][inspection]") {
        TestWorkspaceController controller;
        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrimitive;
        create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        constexpr std::size_t createdCount = 64;
        for (std::size_t index = 0; index < createdCount; ++index)
            controller.ProcessCommand(create);

        const auto endpoint = controller.Controller().BindSceneInspection("test-project", 1).lock();
        REQUIRE(endpoint);
        const auto &objects = controller.ViewModel().objects;
        REQUIRE(objects.size() == createdCount + 1);
        ObjectInspectionRequest request{.limit = 3};
        for (std::size_t index = 0; index < ObjectInspectionRequest::MaximumObjects; ++index)
            request.objects.push_back(objects[objects.size() - 1 - index * 2].id.value);

        const auto first = endpoint->readComponents(request);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().records.size() == request.limit);
        for (std::size_t index = 0; index < request.limit; ++index)
            CHECK(first.Value().records[index].object == request.objects[index]);
        REQUIRE(first.Value().nextOffset == request.limit);
        request.expected = first.Value().scene;
        request.offset = *first.Value().nextOffset;
        const auto second = endpoint->readComponents(request);
        REQUIRE(second.HasValue());
        CHECK(second.Value().records.front().object == request.objects[request.offset]);

        SECTION("Missing objects beyond the returned range reject the entire page") {
            request.objects.back() = objects.back().id.value + 1;
            const auto rejected = endpoint->readComponents(request);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == "object_unavailable");
        }
        SECTION("Duplicate objects beyond the returned range reject the entire page") {
            request.objects.back() = request.objects.front();
            const auto rejected = endpoint->readComponents(request);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == "invalid_page");
        }
        SECTION("An earlier missing object takes precedence over a later invalid ID") {
            request.objects.front() = objects.back().id.value + 1;
            request.objects.back() = 0;
            const auto rejected = endpoint->readComponents(request);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == "object_unavailable");
        }
    }

    /** @brief Seeds a large authored scene in one load so repeated commands do not distort the capture workload. */
    void PrepareInspectionScene(const std::filesystem::path &root, const std::size_t count) {
        const auto path = root / "assets/scenes/main.horo";
        PrepareEmptyDefaultScene(root, path);
        std::vector<SceneSource::SceneObjectSnapshot> objects;
        objects.reserve(count);
        for (std::size_t id = 1; id <= count; ++id) {
            auto &object = objects.emplace_back(SceneSource::SceneObjectSnapshot{.id = {id}, .name = "Inspection object"});
            object.localTransform.translation.x = static_cast<float>(id);
        }
        std::ofstream scene{path, std::ios::binary};
        scene << SceneSource::EncodeSceneSource({objects, {}});
        REQUIRE(scene.good());
    }

    TEST_CASE("Large viewport snapshots preserve ordered bounded transforms and revoke replaced selection",
              "[unit][editor][inspection][viewport-context]") {
        Tests::OwnedTestDirectory project{"viewport-inspection"};
        constexpr std::size_t SceneSize = 4096;
        PrepareInspectionScene(project.Path(), SceneSize);
        TestWorkspaceController workspace{project.Path()};
        REQUIRE(workspace.ViewModel().objects.size() == SceneSize);
        constexpr auto Maximum = AgentSceneSelectionSnapshot::MaximumObjects;
        std::size_t stride = 1;
        SECTION("Clustered selection") {
            // The default stride keeps the selected objects within one contiguous range.
        }
        SECTION("Sparse selection") {
            stride = 241;
        }
        EditorWorkspaceViewCommandData select{.command = EditorWorkspaceViewCommand::SelectObject,
                                              .objectSelection = ObjectSelectionRequest{}};
        for (std::size_t i = 0; i <= Maximum; ++i)
            select.objectSelection->objects.push_back(SceneObjectId{SceneSize - (i * 9 % (Maximum + 1)) * stride});
        select.objectSelection->primary = select.objectSelection->objects.front();
        workspace.ProcessCommand(select);

        auto &controller = workspace.Controller();
        const auto overview = controller.CaptureAgentViewport(AgentViewportComponents::None);
        REQUIRE(overview);
        CHECK(overview->transformCount == 0);
        constexpr std::size_t Samples = 200;
        auto capture = controller.CaptureAgentViewport(AgentViewportComponents::LocalTransforms);
        REQUIRE(capture);
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < Samples; ++i)
            capture = controller.CaptureAgentViewport(AgentViewportComponents::LocalTransforms);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
        UNSCOPED_INFO(
            std::format("{} scoped captures over {} objects at stride {} took {} us", Samples, SceneSize, stride, elapsed.count()));
        REQUIRE(capture);
        CHECK(capture->objectCount == Maximum);
        CHECK(capture->truncated);
        REQUIRE(capture->transformCount == Maximum);
        for (std::size_t i = 0; i < Maximum; ++i) {
            CHECK(capture->objects[i] == select.objectSelection->objects[i].value);
            CHECK(capture->transforms[i].object == capture->objects[i]);
            CHECK(capture->transforms[i].translation[0] == static_cast<float>(capture->objects[i]));
        }

        workspace.ProcessCommand({.command = EditorWorkspaceViewCommand::SelectObject, .objectPayload = SceneObjectId{1}});
        CHECK(capture->lifetime.IsCancellationRequested());
        const auto replacement = controller.CaptureAgentViewport(AgentViewportComponents::LocalTransforms);
        REQUIRE(replacement);
        REQUIRE(replacement->transformCount == 1);
        CHECK(replacement->transforms.front().object == 1);
    }

    TEST_CASE("Viewport image admission binds the authored source through owner and extent changes", "[unit][editor][debug-capture]") {
        CaptureViewport viewport;
        TestWorkspaceController controller;
        auto &workspace = controller.Controller();
        const auto endpoint = workspace.BindDebugCapture("test-project", 1, &viewport).lock();
        REQUIRE(endpoint);
        const auto source = endpoint->readImageSource();
        REQUIRE(source);
        constexpr EditorViewportImageRegion region{2, 3, 24, 16};
        const auto allowed = [] {
            return true;
        };
        REQUIRE(endpoint->startImage(*source, region, allowed).HasValue());
        CHECK(viewport.submitted == region);
        REQUIRE(viewport.current);
        REQUIRE(viewport.current());

        SECTION("camera navigation invalidates the previous approved pixels") {
            EditorWorkspaceViewCommandData navigate;
            navigate.command = EditorWorkspaceViewCommand::NavigateViewport;
            navigate.viewportNavigationPayload = EditorViewportNavigationDelta{.yawRadians = 0.2F};
            controller.ProcessCommand(navigate);
            REQUIRE(endpoint->readImageSource());
            CHECK(endpoint->readImageSource() != source);
        }
        SECTION("resizing leaves the prior approval stale even when its crop still fits") {
            viewport.RequestExtent({800, 600});
            REQUIRE(endpoint->readImageSource());
            CHECK(endpoint->readImageSource()->width == 800);
        }
        SECTION("an authored Scene edit changes the observed projection") {
            EditorWorkspaceViewCommandData create;
            create.command = EditorWorkspaceViewCommand::CreatePrimitive;
            create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
            controller.ProcessCommand(create);
            REQUIRE(endpoint->readImageSource());
            CHECK(endpoint->readImageSource()->sceneRevision != source->sceneRevision);
        }
        SECTION("workspace retirement invalidates even an endpoint retained by a caller") {
            workspace.RevokeWorkspaceCommands();
            CHECK(viewport.cancelledId == 1);
            CHECK_FALSE(endpoint->readImageSource());
        }
        CHECK_FALSE(viewport.current());
        CHECK(endpoint->startImage(*source, region, allowed).HasError());
        CHECK(viewport.requests == 1);
    }

    TEST_CASE("An attempted Play transition requires fresh viewport image approval", "[unit][editor][debug-capture]") {
        Tests::OwnedTestDirectory project{"workspace-viewport-play-approval"};
        PrepareEmptyDefaultScene(project.Path(), project.Path() / "assets/scenes/main.horo");
        CaptureViewport viewport;
        TestWorkspaceController controller{project.Path()};
        auto &workspace = controller.Controller();
        const auto endpoint = workspace.BindDebugCapture("test-project", 1, &viewport).lock();
        REQUIRE(endpoint);
        const auto beforePlay = endpoint->readImageSource();
        REQUIRE(beforePlay);
        constexpr EditorViewportImageRegion region{2, 3, 24, 16};
        const auto allowed = [] {
            return true;
        };
        const auto oldCapture = endpoint->startImage(*beforePlay, region, allowed);
        REQUIRE(oldCapture.HasValue());
        const auto oldAuthority = viewport.current;
        REQUIRE(oldAuthority());

        controller.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::StartPlay));
        CHECK(viewport.cancelledId == oldCapture.Value());

        controller.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::StopPlay));
        REQUIRE(workspace.ViewModel().playState == EditorPlayState::Idle);
        const auto afterPlay = endpoint->readImageSource();
        REQUIRE(afterPlay);
        CHECK(afterPlay != beforePlay);
        CHECK_FALSE(oldAuthority());
        CHECK(endpoint->startImage(*beforePlay, region, allowed).HasError());
        CHECK(viewport.requests == 1);
        REQUIRE(endpoint->startImage(*afterPlay, region, allowed).HasValue());
        CHECK(viewport.requests == 2);
        CHECK(viewport.current());
    }

    TEST_CASE("Replacing capture trust withdraws the old renderer request before any new frame", "[unit][editor][debug-capture]") {
        CaptureViewport viewport;
        TestWorkspaceController controller;
        auto &workspace = controller.Controller();
        const auto first = workspace.BindDebugCapture("test-project", 1, &viewport).lock();
        REQUIRE(first);
        const auto source = first->readImageSource();
        REQUIRE(source);
        const auto allowed = [] {
            return true;
        };
        constexpr EditorViewportImageRegion region{2, 3, 24, 16};
        REQUIRE(first->startImage(*source, region, allowed).HasValue());
        const auto originalAuthority = viewport.current;
        REQUIRE(originalAuthority());

        const auto replacement = workspace.BindDebugCapture("test-project", 2, &viewport).lock();
        REQUIRE(replacement);
        CHECK(viewport.cancelledId == 1);
        CHECK_FALSE(originalAuthority());
        CHECK_FALSE(first->readImageSource());
        REQUIRE(replacement->startImage(*source, region, allowed).HasValue());
        CHECK_FALSE(first->cancelImage(2));
        CHECK(viewport.cancelledId == 1);
        CHECK(viewport.current());
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

    /** @brief Checks that the preview screens peers and retires after its Scene changes. */
    AgentSceneContactReview VerifyPlayContactReview(EditorWorkspaceController &workspace, DecisionNavigationTestHost &host,
                                                    const SceneObjectId selected) {
        REQUIRE(workspace.CanReviewAgentSceneContacts());
        workspace.SelectMountedWorldPlay(true);
        CHECK_FALSE(workspace.CanReviewAgentSceneContacts());
        CHECK_FALSE(workspace.ReadAgentSceneContacts());
        workspace.SelectMountedWorldPlay(false);
        const auto review = workspace.ReadAgentSceneContacts();
        REQUIRE(review);
        CHECK(review->object == selected.value);
        CHECK(review->completedTick == 42);
        REQUIRE(review->count == 3);
        CHECK(review->contacts[0].otherObject == 1);
        CHECK(review->contacts[0].kind == AgentSceneContactKind::TriggerEnter);
        CHECK(review->contacts[1].kind == AgentSceneContactKind::TriggerExit);
        CHECK(review->contacts[2].kind == AgentSceneContactKind::ContactEnd);
        CHECK(workspace.AgentSceneContactsCurrent(*review));
        for (const auto peer : {3ULL, 4ULL, 999999ULL}) {
            host.contactPeer = Runtime::SceneObjectId{peer};
            CHECK_FALSE(workspace.ReadAgentSceneContacts());
        }
        host.contactPeer = Runtime::SceneObjectId{selected.value};
        CHECK_FALSE(workspace.ReadAgentSceneContacts());
        host.contactPeer = Runtime::SceneObjectId{1};
        host.staleContactPeer = true;
        CHECK_FALSE(workspace.ReadAgentSceneContacts());
        host.staleContactPeer = false;
        workspace.SelectMountedWorldPlay(true);
        CHECK_FALSE(workspace.AgentSceneContactsCurrent(*review));
        workspace.SelectMountedWorldPlay(false);
        const auto refreshed = workspace.ReadAgentSceneContacts();
        REQUIRE(refreshed);
        CHECK(workspace.AgentSceneContactsCurrent(*refreshed));
        CHECK_FALSE(workspace.AgentSceneContactsCurrent(*review));
        return *refreshed;
    }

    /** @brief Prevents captured references and contacts from becoming current again after a pause. */
    AgentSceneContactReview VerifyPlayReviewAfterPause(EditorWorkspaceController &workspace, const AgentSceneContactReview &contacts) {
        const auto references = workspace.ReadSelectedPlayReferences();
        REQUIRE(references);
        REQUIRE(workspace.AgentSceneReferencesCurrent(*references));
        REQUIRE(workspace.AgentSceneContactsCurrent(contacts));

        workspace.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::PausePlay));
        REQUIRE(workspace.ViewModel().playState == EditorPlayState::Paused);
        CHECK_FALSE(workspace.AgentSceneReferencesCurrent(*references));
        CHECK_FALSE(workspace.AgentSceneContactsCurrent(contacts));
        CHECK(references->lifetime.IsCancellationRequested());
        CHECK(contacts.lifetime.IsCancellationRequested());

        workspace.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::ResumePlay));
        REQUIRE(workspace.ViewModel().playState == EditorPlayState::Playing);
        CHECK_FALSE(workspace.AgentSceneReferencesCurrent(*references));
        CHECK_FALSE(workspace.AgentSceneContactsCurrent(contacts));
        const auto refreshed = workspace.ReadAgentSceneContacts();
        REQUIRE(refreshed);
        CHECK(refreshed->worldGeneration == contacts.worldGeneration);
        CHECK(workspace.AgentSceneContactsCurrent(*refreshed));
        return *refreshed;
    }

    /** @brief Checks the read-only candidate review against a running editor scene. */
    void VerifyPlayReferenceReview(EditorWorkspaceController &workspace, const Runtime::RuntimeSceneView &play,
                                   const SceneObjectId selected) {
        const auto capturedSelection = workspace.CaptureAgentSceneSelection();
        REQUIRE(capturedSelection);
        const std::array candidates{selected.value, SceneObjectId{1}.value, std::uint64_t{999999}, std::uint64_t{3}, std::uint64_t{4}};
        AgentSceneReferenceRequest request{capturedSelection->documentEpoch, capturedSelection->documentRevision, play.RuntimeId().value,
                                           candidates};
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        request.lifetime = capturedSelection->lifetime;
        const auto references = workspace.ReviewAgentSceneReferences(request);
        REQUIRE(references);
        REQUIRE(references->Candidates().size() == candidates.size());
        CHECK(references->RequiresClarification());
        CHECK(references->Candidates()[0].state == AgentSceneReferenceState::Available);
        CHECK(references->Candidates()[1].state == AgentSceneReferenceState::OutOfScope);
        CHECK(references->Candidates()[2].state == AgentSceneReferenceState::Deleted);
        request.source = AgentSceneReferenceSource::Pointing;
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        request.structuralRevision = play.StructuralRevision();
        const auto pointed = workspace.ReviewAgentSceneReferences(request);
        REQUIRE(pointed);
        CHECK(pointed->RequiresClarification());
        CHECK(pointed->Candidates()[1].state == AgentSceneReferenceState::Available);
        CHECK(pointed->Candidates()[3].state == AgentSceneReferenceState::Hidden);
        CHECK(pointed->Candidates()[4].state == AgentSceneReferenceState::Locked);
        request.source = static_cast<AgentSceneReferenceSource>(0xFF);
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        request.source = AgentSceneReferenceSource::Selection;
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        request.source = AgentSceneReferenceSource::Pointing;
        ++request.structuralRevision;
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        request.structuralRevision = play.StructuralRevision();
        ++request.documentRevision;
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
    }

    /** @brief Exercises the mounted review boundary with the exact running Scene view. */
    void VerifyMountedPlayReferenceReview(EditorWorkspaceController &workspace, const Runtime::RuntimeSceneView &play,
                                          const SceneObjectId object) {
        const auto captured = workspace.CaptureAgentSceneSelection(AgentSceneSelectionPurpose::ImmersivePlay);
        REQUIRE(captured);
        const std::array points{AgentSceneReferencePoint{object.value, {1, 2, 3}}, AgentSceneReferencePoint{999999, {4, 5, 6}}};
        AgentSceneReferenceRequest request{.documentEpoch = captured->documentEpoch,
                                           .documentRevision = captured->documentRevision,
                                           .worldGeneration = play.RuntimeId().value,
                                           .source = AgentSceneReferenceSource::Location,
                                           .locations = points,
                                           .structuralRevision = play.StructuralRevision(),
                                           .mounted = true,
                                           .lifetime = captured->lifetime};
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        workspace.SelectMountedWorldPlay(false);
        CHECK_FALSE(captured->lifetime.IsCancellationRequested());
        workspace.SelectMountedWorldPlay(true);
        CHECK(captured->lifetime.IsCancellationRequested());
        const auto mountedCapture = workspace.CaptureAgentSceneSelection(AgentSceneSelectionPurpose::ImmersivePlay);
        REQUIRE(mountedCapture);
        CHECK_FALSE(mountedCapture->lifetime.IsCancellationRequested());
        workspace.SelectMountedWorldPlay(true);
        CHECK_FALSE(mountedCapture->lifetime.IsCancellationRequested());
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        request.lifetime = mountedCapture->lifetime;
        const auto mounted = workspace.ReviewAgentSceneReferences(request);
        REQUIRE(mounted);
        CHECK(mounted->mounted);
        CHECK(workspace.AgentSceneReferencesCurrent(*mounted));
        REQUIRE(mounted->Candidates().size() == 2);
        CHECK(mounted->RequiresClarification());
        CHECK(mounted->Candidates()[0].worldPosition == points[0].worldPosition);
        CHECK(mounted->Candidates()[1].state == AgentSceneReferenceState::OutOfScope);
        CHECK_FALSE(mounted->Candidates()[1].worldPosition);
        request.mounted = false;
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        request.mounted = true;
        ++request.structuralRevision;
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        workspace.SelectMountedWorldPlay(false);
        CHECK(mountedCapture->lifetime.IsCancellationRequested());
        CHECK_FALSE(workspace.AgentSceneReferencesCurrent(*mounted));
        workspace.SelectMountedWorldPlay(true);
        const auto newCapture = workspace.CaptureAgentSceneSelection(AgentSceneSelectionPurpose::ImmersivePlay);
        REQUIRE(newCapture);
        --request.structuralRevision;
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        CHECK_FALSE(workspace.AgentSceneReferencesCurrent(*mounted));
        request.lifetime = newCapture->lifetime;
        CHECK(workspace.ReviewAgentSceneReferences(request));
        workspace.SelectMountedWorldPlay(false);
    }

    /** @brief Location evidence belongs to its original scene and is withheld for deleted objects. */
    void VerifyPlayLocationReview(EditorWorkspaceController &workspace, const Runtime::RuntimeSceneView &play,
                                  const SceneObjectId selected) {
        const auto captured = workspace.CaptureAgentSceneSelection();
        REQUIRE(captured);
        const std::array locations{AgentSceneReferencePoint{selected.value, {1, 2, 3}}, AgentSceneReferencePoint{selected.value, {1, 2, 4}},
                                   AgentSceneReferencePoint{999999, {5, 6, 7}}};
        const AgentSceneReferenceRequest request{.documentEpoch = captured->documentEpoch,
                                                 .documentRevision = captured->documentRevision,
                                                 .worldGeneration = play.RuntimeId().value,
                                                 .source = AgentSceneReferenceSource::Location,
                                                 .locations = locations,
                                                 .structuralRevision = play.StructuralRevision(),
                                                 .lifetime = captured->lifetime};
        const auto review = workspace.ReviewAgentSceneReferences(request);
        REQUIRE(review);
        REQUIRE(review->Candidates().size() == 3);
        CHECK(review->RequiresClarification());
        CHECK(review->Candidates()[0].worldPosition == locations[0].worldPosition);
        CHECK(review->Candidates()[1].worldPosition == locations[1].worldPosition);
        CHECK(review->Candidates()[2].state == AgentSceneReferenceState::Deleted);
        CHECK_FALSE(review->Candidates()[2].worldPosition);
    }

    /** @brief Verifies that immersive capture can borrow the Play scope with an empty selection. */
    void VerifyUnselectedPlayCapture(EditorWorkspaceController &workspace, const Runtime::SceneRuntimeId scene) {
        REQUIRE(workspace.ViewModel().selectedObjects.empty());
        CHECK_FALSE(workspace.CaptureAgentSceneSelection());
        const auto capture = workspace.CaptureAgentSceneSelection(AgentSceneSelectionPurpose::ImmersivePlay);
        REQUIRE(capture);
        CHECK(capture->objectCount == 0);
        CHECK_FALSE(capture->lifetime.IsCancellationRequested());
        const auto review = workspace.ReviewAgentSceneReferences({.documentEpoch = capture->documentEpoch,
                                                                  .documentRevision = capture->documentRevision,
                                                                  .worldGeneration = scene.value,
                                                                  .lifetime = capture->lifetime});
        REQUIRE(review);
        CHECK(review->Candidates().empty());
        CHECK_FALSE(review->RequiresClarification());
    }

    /** @brief Retires old pointing evidence when the live scene publishes a structural edit. */
    void VerifyPlayPointingAfterStructuralCommit(EditorWorkspaceController &workspace, Runtime::RuntimeSceneService &scenes,
                                                 const SceneObjectId selected) {
        const auto play = scenes.ActiveScene();
        REQUIRE(play);
        const auto retired = play->Find(Runtime::SceneObjectId{3});
        REQUIRE(retired);
        const auto captured = workspace.CaptureAgentSceneSelection();
        REQUIRE(captured);
        const std::array objects{selected.value};
        AgentSceneReferenceRequest request{.documentEpoch = captured->documentEpoch,
                                           .documentRevision = captured->documentRevision,
                                           .worldGeneration = play->RuntimeId().value,
                                           .objects = objects,
                                           .source = AgentSceneReferenceSource::Pointing,
                                           .structuralRevision = play->StructuralRevision(),
                                           .lifetime = captured->lifetime};
        const auto displayed = workspace.ReviewAgentSceneReferences(request);
        REQUIRE(displayed);
        CHECK(workspace.AgentSceneReferencesCurrent(*displayed));

        Runtime::SceneCommandBuffer removal;
        removal.Destroy(*retired);
        REQUIRE(scenes.QueueStructuralCommands(std::move(removal)).HasValue());
        const CancellationToken cancellation;
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 100, .cancellation = cancellation})
                    .HasValue());
        const auto updated = scenes.ActiveScene();
        REQUIRE(updated);
        REQUIRE(updated->Find({selected.value}));
        CHECK_FALSE(workspace.AgentSceneReferencesCurrent(*displayed));
        CHECK_FALSE(workspace.ReviewAgentSceneReferences(request));
        CHECK(updated->StructuralRevision() != request.structuralRevision);
        request.structuralRevision = updated->StructuralRevision();
        const auto refreshed = workspace.ReviewAgentSceneReferences(request);
        REQUIRE(refreshed);
        CHECK(workspace.AgentSceneReferencesCurrent(*refreshed));
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
                                 {.id = original, .name = "AI agent", .components = {.aiAgent = AI::AiAgentComponent{.agent = agent}}},
                                 {.id = SceneObjectId{3}, .name = "Hidden object", .editorState = {.visible = false}},
                                 {.id = SceneObjectId{4}, .name = "Locked object", .editorState = {.locked = true}}}};
        const auto fingerprint = InspectProjectSceneFingerprint(project.Path(), scenePath);
        REQUIRE(fingerprint.HasValue());
        REQUIRE(SaveProjectScene(project.Path(), scenePath, document, fingerprint.Value(), false, mutations, files).HasValue());

        {
            auto sidecar = scenePath;
            sidecar += ".horo";
            std::ofstream stream(sidecar, std::ios::binary);
            stream << nlohmann::json{{"schemaVersion", 1},
                                     {"assetId", Assets::AssetId::FromBytes({0x60}).ToString()},
                                     {"assetType", "core.scene"}}
                          .dump();
            REQUIRE(stream.good());
        }
        using namespace AI::DecisionGraphTestSupport;
        const auto graph = TerminalMachine(701);
        const auto node = std::get<AI::StateMachineAssetDescriptor>(graph.body).states.front().id;
        const auto graphAsset = Assets::AssetId::FromBytes({0x61});
        const auto graphBytes = AI::DecisionPlanCodec::EncodeSource(std::span{&graph, 1}, graph.asset);
        REQUIRE(graphBytes.HasValue());
        const auto writeGraph = [&](std::string_view name, Assets::AssetId asset) {
            const auto path = "assets/" + std::string{name} + ".horo_sm";
            std::ofstream source(project.Path() / path, std::ios::binary);
            source.write(reinterpret_cast<const char *>(graphBytes.Value().data()),
                         static_cast<std::streamsize>(graphBytes.Value().size()));
            REQUIRE(source.good());
            std::ofstream sidecar(project.Path() / (path + ".horo"), std::ios::binary);
            sidecar << nlohmann::json{{"schemaVersion", 1},
                                      {"assetId", asset.ToString()},
                                      {"assetType", Application::DecisionStateMachineSourceType}}
                           .dump();
            REQUIRE(sidecar.good());
        };
        writeGraph("runtime", graphAsset);
        Assets::AssetRegistry registry;
        const auto rebuild = [&] {
            const auto rebuilt = Assets::RebuildAssetRegistry(registry, project.Path(), Assets::AssetRegistryOpenMode::ReadOnly);
            REQUIRE(rebuilt.HasValue());
            for (const auto &diagnostic : rebuilt.Value().diagnostics)
                UNSCOPED_INFO(diagnostic.projectPath.value_or("") << ": " << diagnostic.error.code.Value());
            REQUIRE(rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Complete);
            REQUIRE(registry.Snapshot().Find(graphAsset));
        };
        rebuild();
        CancellationSource cancellation;
        Runtime::RuntimeSceneService preview;
        REQUIRE(preview.Startup(cancellation.Token()).HasValue());
        BuildOutputStore diagnostics{8};
        LocalizationService localization{LocaleTag{"en-US"}};
        REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json"));
        EditorWorkspaceDependencies dependencies;
        dependencies.localization = &localization;
        dependencies.sourceNavigation.diagnosticOutput = &diagnostics;
        dependencies.assets.mutableAssetRegistry = &registry;
        DecisionNavigationTestHost *activeHost{};
        dependencies.gameplay.sceneFactory = [=, &activeHost](Runtime::SceneRuntimeId scene, const Runtime::RuntimeSceneDefinition &) {
            auto host = std::make_unique<DecisionNavigationTestHost>(scene, original, agent, graph.asset, node);
            activeHost = host.get();
            return Result<std::unique_ptr<EditorPlaySceneHost>>::Success(std::move(host));
        };
        EditorWorkspaceController workspace{project.Path(), preview, registry.Snapshot(), dependencies};
        REQUIRE_FALSE(workspace.InitializationError());
        std::vector<NotificationEvent> navigationWarnings;
        const auto subscription = workspace.DataBus().Subscribe<NotificationEvent>([&](const NotificationEvent &notification) {
            if (notification.source == PlayDecisionDiagnosticStage)
                navigationWarnings.push_back(notification);
        });

        const auto play = [&] {
            workspace.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::StartPlay));
            workspace.UpdatePlayPresentation(0.0F);
            INFO(workspace.ViewModel().playError);
            REQUIRE(workspace.ViewModel().playState == EditorPlayState::Playing);
            REQUIRE(workspace.ViewModel().playDecisionProblem);
            REQUIRE(workspace.ViewModel().playDecisionObjectAvailable);
            return *workspace.ViewModel().playDecisionProblem;
        };
        CHECK_FALSE(workspace.CaptureAgentSceneSelection(AgentSceneSelectionPurpose::ImmersivePlay));
        const auto first = play();
        REQUIRE(first.entity);
        VerifyUnselectedPlayCapture(workspace, first.scene);
        workspace.UpdatePlayPresentation(0.0F);
        const auto emitted = diagnostics.SnapshotIfChanged(0);
        REQUIRE(emitted);
        REQUIRE(emitted->records.size() == 1);
        CHECK(emitted->records.front().stage == PlayDecisionDiagnosticStage);
        CHECK(emitted->records.front().code.Value() == first.error.code.Value());
        CHECK_FALSE(emitted->records.front().source);
        EditorWorkspaceViewCommandData graphNavigation{.command = EditorWorkspaceViewCommand::NavigatePlayDecisionGraphProblem,
                                                       .playDecisionGraphNavigation =
                                                           PlayDecisionGraphNavigationRequest{first.scene, first.tick, first.agent,
                                                                                              first.graph, Node(999)}};
        workspace.ProcessCommand(graphNavigation);
        CHECK(workspace.ViewModel().decisionGraphDocuments.empty());
        graphNavigation.playDecisionGraphNavigation->node = node;
        const EditorWorkspaceViewCommandData outputNavigation{
            .command = EditorWorkspaceViewCommand::NavigatePlayDecisionDiagnostic,
            .playDecisionDiagnosticSequence = emitted->records.front().sequence,
        };
        workspace.ProcessCommand(outputNavigation);
        REQUIRE(workspace.ViewModel().decisionGraphDocuments.size() == 1);
        const auto &opened = workspace.ViewModel().decisionGraphDocuments.front();
        CHECK(opened.asset == graphAsset);
        REQUIRE(workspace.ViewModel().playDecisionGraphFocus);
        CHECK(workspace.ViewModel().playDecisionGraphFocus->fence.instance == opened.identity.instance);
        CHECK(workspace.ViewModel().playDecisionGraphFocus->fence.revision == opened.revision);
        CHECK(workspace.ViewModel().playDecisionGraphFocus->node == node);
        CHECK_FALSE(workspace.ViewModel().playDecisionGraphUnavailable);
        CHECK(navigationWarnings.empty());
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
        REQUIRE(activeHost);
        const auto playScene = activeHost->Scenes().ActiveScene();
        REQUIRE(playScene);
        const auto selectionReview = workspace.ReadSelectedPlayReferences();
        REQUIRE(selectionReview);
        REQUIRE(selectionReview->Candidates().size() == 1);
        CHECK(selectionReview->Candidates().front().object == original.value);
        CHECK(selectionReview->Candidates().front().state == AgentSceneReferenceState::Available);
        CHECK(workspace.AgentSceneReferencesCurrent(*selectionReview));
        VerifyPlayReferenceReview(workspace, *playScene, original);
        VerifyPlayLocationReview(workspace, *playScene, original);
        VerifyMountedPlayReferenceReview(workspace, *playScene, original);
        const auto contactReview = VerifyPlayReviewAfterPause(workspace, VerifyPlayContactReview(workspace, *activeHost, original));
        VerifyPlayPointingAfterStructuralCommit(workspace, activeHost->Scenes(), original);
        CHECK_FALSE(workspace.AgentSceneContactsCurrent(contactReview));

        workspace.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::StopPlay));
        CHECK_FALSE(workspace.AgentSceneReferencesCurrent(*selectionReview));
        CHECK_FALSE(workspace.ReadSelectedPlayReferences());
        CHECK_FALSE(workspace.ViewModel().playDecisionProblem);
        const auto second = play();
        CHECK(second.scene != first.scene);
        workspace.ProcessCommand(outputNavigation);
        CHECK_FALSE(workspace.ViewModel().playDecisionGraphFocus);
        CHECK(workspace.ViewModel().contentBrowserOperationError == "workspace.global_dock.build_output.source.invalid");
        REQUIRE(navigationWarnings.size() == 1);
        CHECK(navigationWarnings.front().severity == NotificationSeverity::Warning);
        CHECK(navigationWarnings.front().message == localization.Get("editor", "workspace.global_dock.build_output.source.invalid"));
        workspace.ProcessCommand(graphNavigation);
        CHECK_FALSE(workspace.ViewModel().playDecisionGraphFocus);
        writeGraph("duplicate", Assets::AssetId::FromBytes({0x62}));
        rebuild();
        graphNavigation.playDecisionGraphNavigation->scene = second.scene;
        workspace.ProcessCommand(graphNavigation);
        CHECK_FALSE(workspace.ViewModel().playDecisionGraphFocus);
        CHECK(workspace.ViewModel().playDecisionGraphUnavailable);
        workspace.ProcessCommand({.command = EditorWorkspaceViewCommand::SelectObject,
                                  .objectSelection = ObjectSelectionRequest{.objects = {SceneObjectId{1}}, .primary = SceneObjectId{1}}});
        const auto beforeStaleNavigation = workspace.ViewModel().primarySelection;
        REQUIRE(beforeStaleNavigation != original);
        workspace.ProcessCommand(navigate);
        CHECK(workspace.ViewModel().primarySelection == beforeStaleNavigation);
        workspace.ProcessCommand(SceneCommand(EditorWorkspaceViewCommand::StopPlay));
    }

}  // namespace
