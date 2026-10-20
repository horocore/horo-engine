#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <thread>

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
            std::optional<std::string> navigatedIdentity;
            std::optional<std::filesystem::path> diagnosticPath;
            bool diagnosticRoute{};
            std::unique_ptr<EditorWorkspaceController> controller;

            explicit Workspace(const std::string &sourceName = "hierarchy", const bool useDiagnosticRoute = false)
                : project(sourceName), diagnosticRoute(useDiagnosticRoute) {
                project.WriteScene();
                Open(PrefabAsset(9).Asset());
            }

            void Open(const Assets::AssetId session) {
                controller = std::make_unique<
                    EditorWorkspaceController>(project.directory.path, runtime, project.registry.Snapshot(),
                                               EditorWorkspaceDependencies{.mutations = &project.mutations,
                                                                           .durableFiles = &project.files,
                                                                           .sourceOpenNavigator =
                                                                               diagnosticRoute
                                                                                   ? SourceOpenNavigator{}
                                                                                   : SourceOpenNavigator{[this](const SourceOpenResult
                                                                                                                    &source) {
                    navigated = source.location.absolutePath;
                    navigatedIdentity = source.location.document.Value();
                    return true;
                }},
                                                                           .diagnosticSourceNavigator =
                                                                               [this](const DiagnosticSourceRequest &source) {
                    diagnosticPath = source.absolutePath;
                    return true;
                },
                                                                           .documentSessionOwner = session,
                                                                           .prefabSources = [this](const auto &registry, const auto roots,
                                                                                                   const auto &limits,
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

    TEST_CASE("Workspace prefab source opening preserves native Unicode paths and UTF-8 identity", "[editor][prefab][native]") {
        const std::u8string_view name = u8"köprü_日本_ş";
        Workspace workspace{std::string{name.begin(), name.end()}};
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        REQUIRE(model.prefabHierarchy != nullptr);
        const auto projection = model.prefabHierarchy;
        const auto revision = model.documentRevision;
        owner.ProcessCommand(Intent(model, projection->entries.back().target, PrefabHierarchyAction::OpenSource));
        REQUIRE(workspace.navigated.has_value());
        CHECK(std::filesystem::equivalent(*workspace.navigated, workspace.project.source));
        CHECK(workspace.navigatedIdentity == workspace.project.record.sourcePath.String());
        CHECK(model.prefabHierarchy == projection);
        CHECK(model.documentRevision == revision);
        CHECK_FALSE(model.isDirty);
    }

    TEST_CASE("Workspace prefab diagnostic handoff preserves the actual native Unicode path", "[editor][prefab][native]") {
        const std::u8string_view name = u8"köprü_日本_ş";
        Workspace workspace{std::string{name.begin(), name.end()}, true};
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        REQUIRE(model.prefabHierarchy != nullptr);
        const auto revision = model.documentRevision;
        owner.ProcessCommand(Intent(model, model.prefabHierarchy->entries.back().target, PrefabHierarchyAction::OpenSource));
        REQUIRE(workspace.diagnosticPath.has_value());
        CHECK(*workspace.diagnosticPath == std::filesystem::canonical(workspace.project.source));
        CHECK(std::filesystem::equivalent(*workspace.diagnosticPath, workspace.project.source));
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK(model.documentRevision == revision);
        CHECK_FALSE(model.isDirty);
    }

    TEST_CASE("Workspace diagnostic navigation rejects malformed bytes before opening a document", "[editor][prefab][native]") {
        Workspace workspace{"hierarchy", true};
        auto &owner = *workspace.controller;
        const auto revision = owner.ViewModel().documentRevision;
        const auto projection = owner.ViewModel().prefabHierarchy;
        std::filesystem::path native;
        SECTION("malformed native encoding") {
#ifdef _WIN32
            std::wstring text = L"source/Bad";
            text.push_back(static_cast<wchar_t>(0xd800));
            native = std::filesystem::path{text + L".cpp"};
#else
            native = std::filesystem::path{std::string{"source/Bad"} + "\xc0\xaf" + ".cpp"};
            std::filesystem::create_directories(workspace.project.directory.path / "source");
            std::ofstream(workspace.project.directory.path / native) << "existing invalid native path";
#endif
        }
        SECTION("embedded NUL") {
            native = std::filesystem::path{u8"source/Existing.cpp"};
            std::filesystem::create_directories(workspace.project.directory.path / "source");
            std::ofstream(workspace.project.directory.path / native) << "must not alias";
            native += std::filesystem::path::value_type{};
            native += ".cpp";
        }
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenDiagnosticSource,
                              .diagnosticSource = DiagnosticSourceRequest{.absolutePath = native}});
        CHECK_FALSE(workspace.diagnosticPath.has_value());
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK(owner.ViewModel().contentBrowserOperationError == "workspace.global_dock.build_output.source.invalid");
        CHECK(owner.ViewModel().prefabHierarchy == projection);
        CHECK(owner.ViewModel().documentRevision == revision);
        CHECK_FALSE(owner.ViewModel().isDirty);
    }

    TEST_CASE("Rejected malformed prefab records preserve workspace source authority", "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const auto publication = workspace.project.registry.Snapshot();
        const auto revision = owner.ViewModel().documentRevision;
        const auto projection = owner.ViewModel().prefabHierarchy;
        REQUIRE(projection != nullptr);
        REQUIRE_FALSE(projection->entries.empty());
        std::string name = "broken";
        SECTION("malformed UTF-8") {
            name += "\xc0\xaf";
        }
        SECTION("embedded NUL") {
            name.push_back('\0');
        }
        const auto record = ScenePrefabRecord(workspace.project.asset, name);
        const auto rejected = workspace.project.registry.Publish({record});
        REQUIRE(rejected.status == Assets::AssetRegistryBuildStatus::Failed);
        REQUIRE(rejected.diagnostics.size() == 1);
        CHECK(rejected.diagnostics.front().error.code.Value() == "asset.registry.root_invalid");
        CHECK(rejected.publishedRevision == publication.Revision());
        CHECK(workspace.project.registry.Snapshot().Records().data() == publication.Records().data());
        owner.RefreshAssets(workspace.project.registry.Snapshot());
        const auto &model = owner.ViewModel();
        CHECK(model.prefabHierarchy == projection);
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK_FALSE(workspace.navigatedIdentity.has_value());
        owner.ProcessCommand(Intent(model, projection->entries.front().target, PrefabHierarchyAction::OpenSource));
        REQUIRE(workspace.navigated.has_value());
        CHECK(std::filesystem::equivalent(*workspace.navigated, workspace.project.source));
        CHECK(workspace.navigatedIdentity == workspace.project.record.sourcePath.String());
        CHECK(model.documentRevision == revision);
        CHECK_FALSE(model.isDirty);
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
        Workspace workspace;
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
                                        {.mutations = &project.mutations,
                                         .durableFiles = &project.files,
                                         .documentSessionOwner = PrefabAsset(9).Asset(),
                                         .prefabSources = project.Loader()}};
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
}  // namespace Horo::Editor
