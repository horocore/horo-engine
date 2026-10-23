#include "AllocationProbe.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Horo::Editor {
    namespace {
        using namespace PrefabHierarchyTestSupport;

        /** @brief Observer failure families that must not roll back authoritative committed state. */
        enum class ObserverException {
            Standard,
            Allocation,
            NonStandard
        };

        /** @brief Observer failure deliberately outside the standard exception hierarchy. */
        struct ForeignObserverFailure final {};

        /** @brief Retain and assert the same authoritative commit observations inside and after an observer failure. */
        struct CommitObservation final {
            bool committed{};
            bool pair{};
            bool selection{};
            bool replacement{};

            void Check() const {
                CHECK(committed);
                CHECK(pair);
                CHECK(selection);
                CHECK(replacement);
            }
        };

        /** @brief Composes the production controller with genuine canonical files, registry, history and source capture. */
        struct CreationWorkspace final {
            ProjectDirectory directory;
            std::filesystem::path root{std::filesystem::canonical(directory.path)};
            NativeDurableFileSystem files;
            ProjectMutationCoordinator mutations{files};
            Assets::AssetRegistry registry;
            Runtime::RuntimeSceneService runtime;
            bool failNextPrefabCapture{};
            std::size_t postCommitCaptureFailures{};
            std::unique_ptr<EditorWorkspaceController> controller;

            SceneObjectId selected{11};

            explicit CreationWorkspace(const bool untitled = false) {
                std::filesystem::create_directories(root / ".horo");
                std::filesystem::create_directories(root / "assets");
                const auto release = Application::CurrentEngineReleaseVersion();
                const auto *decision = Application::BuiltInReleaseCompatibilityRegistry().Find(release);
                REQUIRE(decision);
                nlohmann::json metadata{{"horoVersion", Application::FormatHoroVersion(release.value)},
                                        {"persistentContract", Application::FormatPersistentContractHash(decision->persistentContract)},
                                        {"projectId", "prefab-create"},
                                        {"name", "Prefab create"},
                                        {"projectVersion", "0.1.0"},
                                        {"createdAt", "2026-10-20T00:00:00Z"},
                                        {"settings", {{"renderBackend", "opengl"}, {"defaultScene", "assets/main.horo"}}}};
                if (untitled)
                    metadata["settings"]["defaultScene"] = "";
                std::ofstream(root / ".horo/project.json") << metadata.dump();
                const std::vector<SceneObjectSnapshot> objects{{.id = {11}, .name = "Selected subtree"}};
                if (!untitled)
                    std::ofstream(root / "assets/main.horo") << SceneSource::EncodeSceneSource({objects, {}});
                controller =
                    std::make_unique<EditorWorkspaceController>(root, runtime, registry.Snapshot(),
                                                                EditorWorkspaceDependencies{.assets = {.mutableAssetRegistry = &registry},
                                                                                            .world = {.documentSessionOwner =
                                                                                                          PrefabAsset(9).Asset()},
                                                                                            .mutations = &mutations,
                                                                                            .durableFiles = &files,
                                                                                            .prefab = {.sources =
                                                                                                           SourceCapture(release.value)}});
                REQUIRE_FALSE(controller->InitializationError());
                if (untitled)
                    selected = controller->ViewModel().objects.front().id;
                EditorWorkspaceViewCommandData select;
                select.command = EditorWorkspaceViewCommand::SelectObject;
                select.objectPayload = selected;
                controller->ProcessCommand(select);
                REQUIRE(controller->ViewModel().primarySelection == selected);
            }

            /** @brief Supplies the actual canonical capture callback with one-shot postcommit fault injection. */
            PrefabHierarchySourceCapture SourceCapture(const Application::HoroVersion &version) {
                return [this, version](const auto &publication, const auto roots, const auto &limits,
                                       const auto &cancellation) -> Result<PreparedPrefabHierarchySources> {
                    if (failNextPrefabCapture) {
                        failNextPrefabCapture = false;
                        ++postCommitCaptureFailures;
                        throw ForeignObserverFailure{};
                    }
                    auto lease = mutations.TryAcquire({root, ProjectMutationOwner::Asset, "test.prefab.create.capture"});
                    if (lease.HasError())
                        return Result<PreparedPrefabHierarchySources>::Failure(lease.ErrorValue());
                    auto captured = CapturePrefabHierarchySources(root, publication, roots, limits, version, cancellation);
                    if (captured.HasError())
                        return Result<PreparedPrefabHierarchySources>::Failure(captured.ErrorValue());
                    return Result<PreparedPrefabHierarchySources>::Success({std::move(captured).Value(), std::move(lease).Value()});
                };
            }

            void Create() {
                auto captured = controller->CapturePrefabCreationIntent(selected);
                REQUIRE(captured.HasValue());
                auto intent = std::move(captured).Value();
                intent.destination = root / "assets/Created.prefab";
                EditorWorkspaceViewCommandData command;
                command.command = EditorWorkspaceViewCommand::CreatePrefabFromSelection;
                command.prefabCreation = std::move(intent);
                controller->ProcessCommand(command);
            }
        };

        /** @brief Observes every canonical commit owner inside the callback before any injected observer failure. */
        CommitObservation ObserveCommittedCreation(CreationWorkspace &fixture, const SelectionChangedEvent &event,
                                                   const SelectionRevision selectionBefore) {
            const auto &owner = *fixture.controller;
            CommitObservation observation;
            const auto &outcome = owner.ViewModel().prefabCreationOutcome;
            observation.committed = outcome && outcome->committed && !outcome->error;
            observation.pair = std::filesystem::exists(fixture.root / "assets/Created.prefab") &&
                               std::filesystem::exists(fixture.root / "assets/Created.prefab.horo");
            observation.selection = event.kind == SelectionChangeKind::Cleared && event.revision == owner.CurrentSelectionRevision() &&
                                    event.revision > selectionBefore && owner.ViewModel().prefabSelection && outcome &&
                                    owner.ViewModel().prefabSelection->instance == outcome->instance;
            observation.replacement = owner.CapturePrefabCreationIntent(fixture.selected).HasError();
            observation.Check();
            return observation;
        }
    }  // namespace

    TEST_CASE("Workspace prefab creation reconciles selection and one history entry through actual source capture",
              "[editor][prefab][creation][native]") {
        const bool dirty = GENERATE(false, true);
        CreationWorkspace fixture;
        auto &owner = *fixture.controller;
        if (dirty) {
            EditorWorkspaceViewCommandData rename;
            rename.command = EditorWorkspaceViewCommand::UpdateObjectName;
            rename.objectPayload = fixture.selected;
            rename.stringPayload = "Unsaved authoring";
            owner.ProcessCommand(rename);
            REQUIRE(owner.ViewModel().isDirty);
        }
        const auto sceneFocus = owner.ViewModel().workspacePanelHost.ActiveDocument();
        fixture.Create();
        const auto &view = owner.ViewModel();
        CHECK(view.workspacePanelHost.ActiveDocument() == sceneFocus);
        CHECK(view.prefabDocuments.empty());
        REQUIRE(view.prefabCreationOutcome);
        INFO((view.prefabCreationOutcome->error ? view.prefabCreationOutcome->error->message : "committed"));
        REQUIRE(view.prefabCreationOutcome->committed);
        CHECK(view.selectedObjects.empty());
        CHECK_FALSE(view.primarySelection);
        REQUIRE(view.prefabSelection);
        REQUIRE(view.prefabHierarchy);
        REQUIRE(view.prefabHierarchy->sources);
        REQUIRE(view.prefabHierarchy->sources->instances.size() == 1);
        CHECK(view.prefabHierarchy->sources->instances.front().IsSynchronized());
        CHECK(view.isDirty);
        EditorWorkspaceViewCommandData history;
        history.command = EditorWorkspaceViewCommand::UndoScene;
        owner.ProcessCommand(history);
        CHECK(view.objects.size() == 1);
        CHECK(view.isDirty == dirty);
        CHECK(view.objects.front().name == (dirty ? "Unsaved authoring" : "Selected subtree"));
        CHECK(view.prefabHierarchy->sources->instances.empty());
        history.command = EditorWorkspaceViewCommand::RedoScene;
        owner.ProcessCommand(history);
        CHECK(view.objects.empty());
        CHECK(view.prefabHierarchy->sources->instances.size() == 1);
        CHECK(std::filesystem::exists(fixture.root / "assets/Created.prefab"));
    }

    TEST_CASE("Workspace prefab focus rejects Scene creation capture and an earlier Scene confirmation",
              "[editor][prefab][creation][native]") {
        CreationWorkspace fixture;
        auto &owner = *fixture.controller;
        fixture.Create();
        REQUIRE(owner.ViewModel().prefabCreationOutcome->committed);
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        auto captured = owner.CapturePrefabCreationIntent(fixture.selected);
        REQUIRE(captured.HasValue());
        auto intent = std::move(captured).Value();
        intent.destination = fixture.root / "assets/RejectedWhilePrefabFocused.prefab";
        owner.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::OpenSourceFile,
             .sourceOpenRequest = SourceOpenRequest{.path = fixture.root / "assets/Created.prefab", .origin = SourceOpenOrigin::Command}});
        REQUIRE(owner.ViewModel().prefabDocuments.size() == 1);
        const auto &prefab = owner.ViewModel().prefabDocuments.front();
        REQUIRE(owner.ViewModel().workspacePanelHost.ActiveDocument() == prefab.identity.instance);
        CHECK(owner.CapturePrefabCreationIntent(fixture.selected).HasError());
        const auto sceneRevision = owner.ViewModel().documentRevision;
        const auto prefabRevision = prefab.fence.revision;
        const auto priorOutcome = owner.ViewModel().prefabCreationOutcome;
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::CreatePrefabFromSelection, .prefabCreation = intent});
        CHECK(owner.ViewModel().prefabCreationOutcome == priorOutcome);
        CHECK(owner.ViewModel().documentRevision == sceneRevision);
        CHECK(owner.ViewModel().prefabDocuments.front().fence.revision == prefabRevision);
        CHECK(owner.ViewModel().objects.size() == 1);
        CHECK_FALSE(std::filesystem::exists(intent.destination));
        CHECK_FALSE(std::filesystem::exists(intent.destination.string() + ".horo"));
    }

    TEST_CASE("Workspace creation revalidates protected objects and project metadata before writing any source",
              "[editor][prefab][creation][native]") {
        const bool locked = GENERATE(false, true);
        CreationWorkspace fixture;
        auto &owner = *fixture.controller;
        auto captured = owner.CapturePrefabCreationIntent(fixture.selected);
        REQUIRE(captured.HasValue());
        auto intent = std::move(captured).Value();
        intent.destination = fixture.root / "assets/Rejected.prefab";
        if (locked) {
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::UpdateObjectEditorState;
            command.objectPayload = fixture.selected;
            command.editorStatePayload = SceneObjectEditorState{.visible = true, .locked = true};
            owner.ProcessCommand(command);
            REQUIRE(owner.ViewModel().objects.front().effectivelyLocked);
            CHECK(owner.CapturePrefabCreationIntent(fixture.selected).HasError());
        } else {
            REQUIRE(std::filesystem::remove(fixture.root / ".horo/project.json"));
        }
        const auto revision = owner.CurrentSelectionRevision();
        const bool dirty = owner.ViewModel().isDirty;
        const bool undo = owner.ViewModel().canUndo;
        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrefabFromSelection;
        create.prefabCreation = std::move(intent);
        owner.ProcessCommand(create);
        const auto &view = owner.ViewModel();
        REQUIRE(view.prefabCreationOutcome);
        CHECK_FALSE(view.prefabCreationOutcome->committed);
        CHECK(view.prefabCreationOutcome->error.has_value());
        CHECK(owner.CurrentSelectionRevision() == revision);
        CHECK(view.primarySelection == fixture.selected);
        CHECK(view.objects.size() == 1);
        CHECK(view.isDirty == dirty);
        CHECK(view.canUndo == undo);
        CHECK_FALSE(view.prefabSelection);
        CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/Rejected.prefab"));
        CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/Rejected.prefab.horo"));
    }

    TEST_CASE("Workspace postcommit observer failures cannot delete the created prefab or revert the linked Scene",
              "[editor][prefab][creation][native]") {
        const auto exception = GENERATE(ObserverException::Standard, ObserverException::Allocation, ObserverException::NonStandard);
        CreationWorkspace fixture;
        auto &owner = *fixture.controller;
        std::optional<Tests::AllocationProbe::ScopedFailure> failure;
        std::size_t observed{};
        const auto selectionBefore = owner.CurrentSelectionRevision();
        CommitObservation observation;
        auto subscription = owner.DataBus().Subscribe<SelectionChangedEvent>([&](const SelectionChangedEvent &event) {
            ++observed;
            observation = ObserveCommittedCreation(fixture, event, selectionBefore);
            if (exception == ObserverException::Allocation)
                failure.emplace(0);
            else if (exception == ObserverException::NonStandard)
                fixture.failNextPrefabCapture = true;
            else
                throw std::runtime_error{"postcommit observer"};
        });
        fixture.Create();
        failure.reset();
        REQUIRE(observed == 1);
        observation.Check();
        REQUIRE(owner.ViewModel().prefabCreationOutcome);
        CHECK(owner.ViewModel().prefabCreationOutcome->committed);
        CHECK_FALSE(owner.ViewModel().prefabCreationOutcome->error);
        CHECK(owner.ViewModel().prefabSelection.has_value());
        CHECK(std::filesystem::exists(fixture.root / "assets/Created.prefab"));
        CHECK(std::filesystem::exists(fixture.root / "assets/Created.prefab.horo"));
        if (exception != ObserverException::Standard)
            CHECK(owner.ViewModel().prefabPostCommitError.has_value());
        CHECK(fixture.postCommitCaptureFailures == (exception == ObserverException::NonStandard ? 1 : 0));
        EditorWorkspaceViewCommandData history;
        history.command = EditorWorkspaceViewCommand::UndoScene;
        owner.ProcessCommand(history);
        CHECK(owner.ViewModel().objects.size() == 1);
        history.command = EditorWorkspaceViewCommand::RedoScene;
        owner.ProcessCommand(history);
        CHECK(owner.ViewModel().objects.empty());
        CHECK(owner.ViewModel().prefabHierarchy->sources->instances.size() == 1);
    }

    TEST_CASE("Workspace rejects a filename confirmation from a replaced or edited Scene without changing selection",
              "[editor][prefab][creation][native]") {
        CreationWorkspace fixture;
        auto &owner = *fixture.controller;
        auto stale = owner.CapturePrefabCreationIntent({11}).Value();
        stale.destination = fixture.root / "assets/Stale.prefab";
        EditorWorkspaceViewCommandData rename;
        rename.command = EditorWorkspaceViewCommand::UpdateObjectName;
        rename.objectPayload = SceneObjectId{11};
        rename.stringPayload = "Later edit";
        owner.ProcessCommand(rename);
        const auto before = owner.ViewModel().documentRevision;
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::CreatePrefabFromSelection;
        command.prefabCreation = stale;
        owner.ProcessCommand(command);
        REQUIRE(owner.ViewModel().prefabCreationOutcome);
        CHECK_FALSE(owner.ViewModel().prefabCreationOutcome->committed);
        CHECK(owner.ViewModel().documentRevision == before);
        CHECK(owner.ViewModel().primarySelection == SceneObjectId{11});
        CHECK_FALSE(std::filesystem::exists(stale.destination));
    }

    TEST_CASE("Workspace prefab creation detects persisted Scene edits before the background watcher reports them",
              "[editor][prefab][creation][native]") {
        CreationWorkspace fixture;
        auto &owner = *fixture.controller;
        const auto before = owner.ViewModel().documentRevision;
        std::ofstream(fixture.root / "assets/main.horo", std::ios::trunc) << "foreign unobserved Scene edit";
        CHECK_FALSE(owner.ViewModel().sceneExternalConflict);
        fixture.Create();
        REQUIRE(owner.ViewModel().prefabCreationOutcome);
        CHECK_FALSE(owner.ViewModel().prefabCreationOutcome->committed);
        REQUIRE(owner.ViewModel().prefabCreationOutcome->error);
        CHECK(owner.ViewModel().documentRevision == before);
        CHECK(owner.ViewModel().primarySelection == SceneObjectId{11});
        CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/Created.prefab"));
    }

    TEST_CASE("Workspace prefab creation distinguishes untitled authoring from a missing persisted Scene",
              "[editor][prefab][creation][native]") {
        const bool untitled = GENERATE(false, true);
        CreationWorkspace fixture{untitled};
        auto &owner = *fixture.controller;
        if (!untitled)
            REQUIRE(std::filesystem::remove(fixture.root / "assets/main.horo"));
        fixture.Create();
        REQUIRE(owner.ViewModel().prefabCreationOutcome);
        CHECK(owner.ViewModel().prefabCreationOutcome->committed == untitled);
        CHECK(std::filesystem::exists(fixture.root / "assets/Created.prefab") == untitled);
        CHECK(owner.CurrentScenePath().has_value() != untitled);
    }

}  // namespace Horo::Editor
