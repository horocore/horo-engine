#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {
    using namespace PrefabHierarchyTestSupport;

    namespace {
        /** @brief Captures one actual owner publication as a panel would, without a document mutation capability. */
        EditorWorkspaceViewCommandData Intent(const EditorWorkspaceViewModel &model, PrefabHierarchyTarget target,
                                              PrefabHierarchyAction action) {
            REQUIRE(model.prefabHierarchy != nullptr);
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::PrefabHierarchy;
            command.prefabHierarchy = PrefabHierarchyCommand{model.prefabHierarchy->documentSession, model.prefabHierarchy->publication,
                                                             model.prefabHierarchy->documentRevision, target, action};
            return command;
        }

        /** @brief Exercises actual Scene persistence, source pair capture, canonical history and workspace routes. */
        struct Workspace final {
            Project project;
            Runtime::RuntimeSceneService runtime;
            std::size_t captures{};
            std::optional<std::filesystem::path> navigated;
            std::unique_ptr<EditorWorkspaceController> controller;

            Workspace() {
                project.WriteScene();
                Open(PrefabAsset(9).Asset());
            }

            void Open(const Assets::AssetId session) {
                controller =
                    std::make_unique<EditorWorkspaceController>(project.directory.path, runtime, project.registry.Snapshot(),
                                                                EditorWorkspaceDependencies{.mutations = &project.mutations,
                                                                                            .durableFiles = &project.files,
                                                                                            .sourceOpenNavigator =
                                                                                                [this](const SourceOpenResult &source) {
                    navigated = source.location.absolutePath;
                    return true;
                },
                                                                                            .documentSessionOwner = session,
                                                                                            .prefabSources =
                                                                                                [this](const auto &registry,
                                                                                                       const auto roots, const auto &limits,
                                                                                                       const auto &cancellation) {
                    ++captures;
                    return project.Capture(registry, roots, limits, cancellation);
                }});
                REQUIRE_FALSE(controller->InitializationError().has_value());
            }
        };
    }  // namespace

    TEST_CASE("Workspace prefab selection and source navigation retain exact ownership without ordinary mutation",
              "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        REQUIRE(model.prefabHierarchy != nullptr);
        REQUIRE(model.prefabHierarchy->entries.size() == 3);
        const auto member = model.prefabHierarchy->entries.back().target;
        const auto revision = model.documentRevision;
        const auto cache = model.prefabHierarchy;
        owner.ProcessCommand(Intent(model, member, PrefabHierarchyAction::Select));
        CHECK(model.prefabSelection == member);
        CHECK_FALSE(model.primarySelection.has_value());
        CHECK(model.selectedObjects.empty());
        CHECK(model.documentRevision == revision);
        CHECK(model.prefabHierarchy == cache);
        CHECK(workspace.captures == 1);
        owner.ProcessCommand(Intent(model, member, PrefabHierarchyAction::DeleteInstance));
        CHECK(model.documentRevision == revision);
        CHECK_FALSE(model.isDirty);
        owner.ProcessCommand(Intent(model, member, PrefabHierarchyAction::OpenSource));
        REQUIRE(workspace.navigated.has_value());
        CHECK(std::filesystem::equivalent(*workspace.navigated, workspace.project.source));
        owner.ProcessCommand(Intent(model, member, PrefabHierarchyAction::LocateInstance));
        REQUIRE(model.prefabSelection.has_value());
        CHECK_FALSE(model.prefabSelection->member.has_value());
        CHECK(model.prefabSelection->instance == workspace.project.instance);
    }

    TEST_CASE("Workspace prefab root deletion uses canonical undo redo save and reload", "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const PrefabHierarchyTarget root{workspace.project.instance, std::nullopt};
        auto stale = Intent(owner.ViewModel(), root, PrefabHierarchyAction::DeleteInstance);
        owner.ProcessCommand(stale);
        CHECK(owner.ViewModel().isDirty);
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.empty());
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.size() == 3);
        const auto revision = owner.ViewModel().documentRevision;
        owner.ProcessCommand(stale);
        CHECK(owner.ViewModel().documentRevision == revision);
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::RedoScene});
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.empty());
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::SaveScene});
        CHECK_FALSE(owner.ViewModel().isDirty);
        workspace.controller.reset();
        workspace.Open(PrefabAsset(10).Asset());
        REQUIRE(workspace.controller->ViewModel().prefabHierarchy->entries.empty());
        workspace.controller->ProcessCommand(stale);
        CHECK_FALSE(workspace.controller->ViewModel().isDirty);
    }

    TEST_CASE("Workspace prefab source retirement preserves repairable roots on actual registry refresh", "[editor][prefab][native]") {
        Workspace workspace;
        const auto prior = workspace.controller->ViewModel().prefabHierarchy;
        const auto stale = Intent(workspace.controller->ViewModel(), prior->entries.back().target, PrefabHierarchyAction::OpenSource);
        const auto revision = workspace.controller->ViewModel().documentRevision;
        REQUIRE(std::filesystem::remove(workspace.project.source));
        REQUIRE(workspace.project.registry.Publish({workspace.project.record}).status == Assets::AssetRegistryBuildStatus::Complete);
        workspace.controller->RefreshAssets(workspace.project.registry.Snapshot());
        const auto current = workspace.controller->ViewModel().prefabHierarchy;
        REQUIRE(current != nullptr);
        REQUIRE(current->entries.size() == 3);
        CHECK(current->entries.front().target.instance == workspace.project.instance);
        REQUIRE(current->entries.front().failure.has_value());
        CHECK(prior->entries.size() == 3);
        CHECK(workspace.controller->ViewModel().documentRevision == revision);
        CHECK(current->publication != prior->publication);
        workspace.controller->ProcessCommand(stale);
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK_FALSE(workspace.controller->ViewModel().isDirty);
    }

    TEST_CASE("Workspace prefab root movement and duplication retain one canonical history and reject member parents",
              "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const PrefabHierarchyTarget root{workspace.project.instance, std::nullopt};
        owner.ProcessCommand(Intent(owner.ViewModel(), root, PrefabHierarchyAction::DuplicateInstance));
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.size() == 6);
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.size() == 3);
        auto rejected = Intent(owner.ViewModel(), root, PrefabHierarchyAction::ReparentInstance);
        rejected.prefabHierarchy->destinationParent = SceneObjectId{999};
        const auto prior = owner.ViewModel().documentRevision;
        owner.ProcessCommand(rejected);
        CHECK(owner.ViewModel().documentRevision == prior);
        auto moved = Intent(owner.ViewModel(), root, PrefabHierarchyAction::ReparentInstance);
        owner.ProcessCommand(moved);
        CHECK_FALSE(owner.ViewModel().prefabHierarchy->sources->instances.front().authored.parent.has_value());
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        CHECK(owner.ViewModel().prefabHierarchy->sources->instances.front().authored.parent == SceneObjectId{11});
    }

    TEST_CASE("Closed workspace prefab gestures cannot resolve a fresh owner with the same saved scene", "[editor][prefab][native]") {
        Workspace workspace;
        const PrefabHierarchyTarget root{workspace.project.instance, std::nullopt};
        auto stale = Intent(workspace.controller->ViewModel(), root, PrefabHierarchyAction::DeleteInstance);
        const auto retained = workspace.controller->ViewModel().prefabHierarchy;
        workspace.controller.reset();
        REQUIRE(retained->entries.size() == 3);
        workspace.Open(PrefabAsset(10).Asset());
        const auto before = workspace.controller->ViewModel().documentRevision;
        REQUIRE(workspace.controller->ViewModel().prefabHierarchy->entries.size() == 3);
        workspace.controller->ProcessCommand(stale);
        CHECK(workspace.controller->ViewModel().documentRevision == before);
        CHECK_FALSE(workspace.controller->ViewModel().isDirty);
        CHECK(workspace.controller->ViewModel().prefabHierarchy->entries.size() == 3);
    }

    TEST_CASE("Prefab gesture publication survives SaveCopy and rejected SaveAs but retires on successful SaveAs",
              "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const PrefabHierarchyTarget root{workspace.project.instance, std::nullopt};
        auto stale = Intent(owner.ViewModel(), root, PrefabHierarchyAction::Select);
        const auto publication = owner.ViewModel().prefabHierarchy;
        EditorWorkspaceViewCommandData save;
        save.command = EditorWorkspaceViewCommand::SaveSceneCopyAs;
        save.stringPayload = (workspace.project.directory.path / "assets/scenes/copy.horo").string();
        owner.ProcessCommand(save);
        CHECK(owner.ViewModel().prefabHierarchy == publication);
        save.command = EditorWorkspaceViewCommand::SaveSceneAs;
        save.stringPayload = (workspace.project.directory.path.parent_path() / "outside.horo").string();
        owner.ProcessCommand(save);
        CHECK(owner.ViewModel().prefabHierarchy == publication);
        save.stringPayload = (workspace.project.directory.path / "assets/scenes/moved.horo").string();
        owner.ProcessCommand(save);
        REQUIRE(owner.ViewModel().prefabHierarchy != nullptr);
        CHECK(owner.ViewModel().prefabHierarchy->publication != publication->publication);
        owner.ProcessCommand(stale);
        CHECK_FALSE(owner.ViewModel().prefabSelection.has_value());
        CHECK_FALSE(owner.ViewModel().isDirty);
    }
}  // namespace Horo::Editor
