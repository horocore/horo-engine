#include "PrefabWorkspaceTestSupport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace Horo::Editor {
    using namespace PrefabWorkspaceTestSupport;

    namespace {
        /** @brief Copies the exact source gesture emitted by Assets through the ordinary shared drop route. */
        EditorWorkspaceViewCommandData Drop(const Workspace &workspace, const AssetSceneDropTarget target,
                                            const EditorWorkspaceViewCommand action = EditorWorkspaceViewCommand::InstantiateAsset) {
            const auto &model = workspace.controller->ViewModel();
            const auto native = workspace.project.source.generic_u8string();
            EditorWorkspaceViewCommandData command;
            command.command = action;
            command.assetSceneDrop = AssetSceneDropRequest{.assetId = workspace.project.asset.ToString(),
                                                           .assetType = "core.prefab",
                                                           .absoluteAssetPath = std::string{native.begin(), native.end()},
                                                           .target = target,
                                                           .documentRevision = model.documentRevision,
                                                           .fence = model.assetDropFence};
            return command;
        }
    }  // namespace

    TEST_CASE("Assets prefab drops share canonical placement history dirty save and undo", "[editor][prefab][asset-drop][native]") {
        const std::u8string_view sourceName = u8"köprü_日本 prefab with spaces";
        Workspace workspace{std::string{sourceName.begin(), sourceName.end()}};
        auto &owner = *workspace.controller;
        auto drop = Drop(workspace, AssetSceneDropTarget::HierarchyRoot);
        SECTION("hierarchy child") {
            drop.assetSceneDrop->target = AssetSceneDropTarget::HierarchyChild;
            drop.assetSceneDrop->parent = SceneObjectId{11};
        }
        SECTION("hierarchy sibling") {
            drop.assetSceneDrop->target = AssetSceneDropTarget::HierarchySibling;
        }
        SECTION("hierarchy root") {}
        const auto before = owner.ViewModel().documentRevision;
        owner.ProcessCommand(drop);
        const auto &model = owner.ViewModel();
        CHECK(model.documentRevision.value == before.value + 1);
        CHECK(model.isDirty);
        CHECK(model.prefabDocuments.empty());
        REQUIRE(model.prefabSelection.has_value());
        REQUIRE(model.prefabHierarchy->sources->instances.size() == 2);
        const auto placed = model.prefabHierarchy->sources->instances.back().authored;
        CHECK(placed.sourcePrefab.Asset() == workspace.project.asset);
        CHECK(placed.parent == drop.assetSceneDrop->parent);
        CHECK(placed.rootTransform == Math::Transform{});
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        CHECK_FALSE(model.canUndo);
        CHECK_FALSE(model.isDirty);
        REQUIRE(model.prefabHierarchy->sources->instances.size() == 1);
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::RedoScene});
        REQUIRE(model.prefabHierarchy->sources->instances.size() == 2);
        CHECK(model.prefabHierarchy->sources->instances.back().authored == placed);
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::SaveScene});
        CHECK_FALSE(model.isDirty);
        workspace.controller.reset();
        workspace.Open(PrefabAsset(10).Asset());
        REQUIRE(workspace.controller->ViewModel().prefabHierarchy->sources->instances.size() == 2);
        CHECK(workspace.controller->ViewModel().prefabHierarchy->sources->instances.back().authored == placed);
    }

    TEST_CASE("Rejected prefab source and lifecycle drops preserve scene selection and semantic history",
              "[editor][prefab][asset-drop][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        owner.ProcessCommand(Intent(owner.ViewModel(), {workspace.project.instance, std::nullopt}, PrefabHierarchyAction::Select));
        auto drop = Drop(workspace, AssetSceneDropTarget::HierarchyRoot);
        const auto selection = owner.ViewModel().prefabSelection;
        const auto publication = owner.ViewModel().prefabHierarchy;
        const auto revision = owner.ViewModel().documentRevision;
        SECTION("missing source") {
            REQUIRE(std::filesystem::remove(workspace.project.source));
        }
        SECTION("malformed source") {
            std::ofstream(workspace.project.source, std::ios::trunc) << "{";
        }
        SECTION("unregistered identity") {
            drop.assetSceneDrop->assetId = PrefabAsset(99).Asset().ToString();
        }
        SECTION("wrong type") {
            drop.assetSceneDrop->assetType = "core.mesh";
        }
        SECTION("foreign path") {
            drop.assetSceneDrop->absoluteAssetPath = (workspace.project.directory.path / "other.prefab").string();
        }
        SECTION("missing parent") {
            drop.assetSceneDrop->parent = SceneObjectId{999};
        }
        SECTION("foreign session") {
            drop.assetSceneDrop->fence.session = PrefabAsset(99).Asset();
        }
        SECTION("replacement opening") {
            ++drop.assetSceneDrop->fence.replacementEpoch;
        }
        SECTION("old source gesture revision") {
            ++drop.assetSceneDrop->fence.revision.value;
        }
        SECTION("external scene write before watcher notification") {
            std::ofstream(workspace.project.directory.path / "assets/scenes/main.horo", std::ios::app) << " ";
        }
        owner.ProcessCommand(drop);
        CHECK(owner.ViewModel().documentRevision == revision);
        CHECK(owner.ViewModel().prefabHierarchy == publication);
        CHECK(owner.ViewModel().prefabSelection == selection);
        CHECK_FALSE(owner.ViewModel().isDirty);
        CHECK_FALSE(owner.ViewModel().canUndo);
    }

    TEST_CASE("Prefab viewport ghost and final placement share world transform and exclude self hits",
              "[editor][prefab][asset-drop][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        auto preview = Drop(workspace, AssetSceneDropTarget::Viewport, EditorWorkspaceViewCommand::PreviewAssetPlacement);
        const auto originalCount = owner.ViewportScene().instances.size();
        const auto revision = owner.ViewModel().documentRevision;
        owner.ProcessCommand(preview);
        REQUIRE(owner.ViewportScene().instances.size() > originalCount);
        const auto first = owner.ViewportScene().instances.back().localToWorld;
        CHECK(owner.ViewportScene().instancePickable.back() == 0U);
        CHECK(owner.ViewModel().documentRevision == revision);
        CHECK_FALSE(owner.ViewModel().canUndo);
        owner.ProcessCommand(preview);
        CHECK(owner.ViewportScene().instances.back().localToWorld == first);
        CHECK(workspace.captures == 2);  // Initial scene capture plus one cached drag closure.
        SECTION("final commit") {
            preview.command = EditorWorkspaceViewCommand::InstantiateAsset;
            owner.ProcessCommand(preview);
            REQUIRE(owner.ViewModel().prefabHierarchy->sources->instances.size() == 2);
            const auto &placed = owner.ViewModel().prefabHierarchy->sources->instances.back().authored;
            CHECK(placed.rootTransform.ToMatrix() == first);
            CHECK(owner.ViewportScene().instances.size() == originalCount);
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
            CHECK_FALSE(owner.ViewModel().canUndo);
        }
        SECTION("project metadata changed after preview") {
            std::ofstream(workspace.project.directory.path / ".horo/project.json", std::ios::app) << " ";
            preview.command = EditorWorkspaceViewCommand::InstantiateAsset;
            owner.ProcessCommand(preview);
            CHECK(owner.ViewModel().documentRevision == revision);
            CHECK_FALSE(owner.ViewModel().canUndo);
            CHECK(owner.ViewportScene().instances.size() == originalCount);
        }
        SECTION("source changed after preview") {
            workspace.project.WriteSource("Replacement");
            preview.command = EditorWorkspaceViewCommand::InstantiateAsset;
            owner.ProcessCommand(preview);
            CHECK(owner.ViewModel().documentRevision == revision);
            CHECK_FALSE(owner.ViewModel().canUndo);
            CHECK(owner.ViewportScene().instances.size() == originalCount);
        }
        SECTION("cancel") {
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::CancelAssetPlacementPreview});
            CHECK(owner.ViewportScene().instances.size() == originalCount);
            CHECK(owner.ViewModel().documentRevision == revision);
            CHECK_FALSE(owner.ViewModel().canUndo);
        }
    }

    TEST_CASE("Placed prefab reaches the ordinary runtime viewport pipeline", "[editor][prefab][asset-drop][native]") {
        Project project;
        project.WriteScene();
        Runtime::RuntimeSceneService runtime;
        CancellationSource cancellation;
        REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
        EditorWorkspaceController owner{project.directory.path,
                                        runtime,
                                        project.registry.Snapshot(),
                                        {.world = {.documentSessionOwner = PrefabAsset(9).Asset()},
                                         .mutations = &project.mutations,
                                         .durableFiles = &project.files,
                                         .prefab = {.sources = project.Loader()}}};
        REQUIRE_FALSE(owner.InitializationError().has_value());
        const auto source = project.source.generic_u8string();
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::InstantiateAsset,
                              .assetSceneDrop = AssetSceneDropRequest{.assetId = project.asset.ToString(),
                                                                      .assetType = "core.prefab",
                                                                      .absoluteAssetPath = std::string{source.begin(), source.end()},
                                                                      .target = AssetSceneDropTarget::HierarchyRoot,
                                                                      .documentRevision = owner.ViewModel().documentRevision,
                                                                      .fence = owner.ViewModel().assetDropFence}});
        REQUIRE(owner.ViewModel().isDirty);
        for (int frame = 0; frame < 2000; ++frame) {
            REQUIRE(runtime
                        .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                 Runtime::FrameContext{1, {}, 0.0, 0, {}, false, cancellation.Token()})
                        .HasValue());
            owner.SynchronizeRuntimeScenePreview();
            if (owner.ViewportScene().documentRevision == owner.ViewModel().documentRevision)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(runtime.ActiveScene().has_value());
        CHECK(runtime.ActiveScene()->SlotCount() == 5);  // Ordinary parent plus two complete two-object prefab instances.
        CHECK(owner.ViewportScene().documentRevision == owner.ViewModel().documentRevision);
        CHECK_FALSE(runtime.TakeOperationError().has_value());
    }

    TEST_CASE("Prefab document focus rejects captured Scene asset placement intents", "[editor][prefab][asset-drop][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        auto drop = Drop(workspace, AssetSceneDropTarget::HierarchyRoot);
        workspace.OpenPrefab();
        const auto &model = owner.ViewModel();
        REQUIRE(model.prefabDocuments.size() == 1);
        const auto prefab = model.prefabDocuments.front();
        const auto sceneRevision = model.documentRevision;
        const auto hierarchy = model.prefabHierarchy;
        const auto captures = workspace.captures;
        owner.ProcessCommand(drop);
        drop.command = EditorWorkspaceViewCommand::PreviewAssetPlacement;
        owner.ProcessCommand(drop);
        CHECK(model.documentRevision == sceneRevision);
        CHECK(model.prefabHierarchy == hierarchy);
        CHECK_FALSE(model.isDirty);
        CHECK_FALSE(model.canUndo);
        CHECK(workspace.captures == captures);
        REQUIRE(model.prefabDocuments.size() == 1);
        CHECK(model.prefabDocuments.front().fence.owner == prefab.fence.owner);
        CHECK(model.prefabDocuments.front().fence.instance == prefab.fence.instance);
        CHECK(model.prefabDocuments.front().fence.revision == prefab.fence.revision);
        CHECK(model.prefabDocuments.front().source == prefab.source);
        CHECK_FALSE(model.prefabDocuments.front().dirty);
    }

}  // namespace Horo::Editor
