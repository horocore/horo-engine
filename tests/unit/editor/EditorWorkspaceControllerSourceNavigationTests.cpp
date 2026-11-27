#include "../../support/AllocationProbe.h"
#include "../../support/OwnedTestDirectory.h"
#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/modals/source_document/SourceDocumentCloseModal.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceScreen.h"
#include "editor/screens/workspace/gameplay/GameplayBehaviorRequestValidation.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdint>
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

    TEST_CASE("Diagnostic source navigation rejects files outside the project", "[unit][editor][diagnostics]") {
        const std::filesystem::path base =
            std::filesystem::temp_directory_path() /
            ("horo-diagnostic-source-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path projectRoot = base / "project";
        const std::filesystem::path outsideSource = base / "outside.cpp";
        std::filesystem::create_directories(projectRoot);
        std::filesystem::create_directories(projectRoot / "source");
        {
            std::ofstream source(outsideSource);
            source << "int main() {}\n";
        }

        TestWorkspaceController controller{projectRoot.string()};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenDiagnosticSource;
        command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = outsideSource, .line = 1};
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.global_dock.build_output.source.invalid"));

        command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = projectRoot / "source/Missing.cpp", .line = 3};
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.source_open.missing"));

        const std::filesystem::path escapingLink = projectRoot / "source/escape.cpp";
        std::error_code symlinkError;
        std::filesystem::create_symlink(outsideSource, escapingLink, symlinkError);
        if (!symlinkError) {
            command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = escapingLink, .line = 4};
            controller.ProcessCommand(command);
            REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.global_dock.build_output.source.invalid"));
        }

        std::error_code cleanupError;
        std::filesystem::remove_all(base, cleanupError);
    }

    TEST_CASE("Diagnostic source navigation preserves a validated line and column", "[unit][editor][diagnostics]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-diagnostic-location-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path sourcePath = projectRoot / std::filesystem::path{u8"assets/shaders/ışık_日本.glsl"};
        std::filesystem::create_directories(sourcePath.parent_path());
        {
            std::ofstream source(sourcePath);
            source << "void main() {}\n";
        }

        std::optional<DiagnosticSourceRequest> navigated;
        TestWorkspaceController controller{projectRoot.string(), [&navigated](const DiagnosticSourceRequest &source) {
            navigated = source;
            return true;
        }};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenDiagnosticSource;
        command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = sourcePath, .line = 27, .column = 9};
        controller.ProcessCommand(command);

        REQUIRE(navigated.has_value());
        REQUIRE((std::filesystem::weakly_canonical(navigated->absolutePath) == std::filesystem::weakly_canonical(sourcePath)));
        REQUIRE((navigated->line == 27));
        REQUIRE((navigated->column == 9));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    /** @brief Removes the test project after controller and snapshot owners have been destroyed. */
    struct ScopedSourceProjectCleanup final {
        const std::filesystem::path &path;

        ~ScopedSourceProjectCleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    /** @brief Exercises actual Save As and explicit close commands after a conflicted Save All. */
    void CheckSourceRelocationAndClose(TestWorkspaceController &controller, const SourceDocumentSnapshot &saved,
                                       std::uint64_t editedRevision) {
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::SaveSourceDocumentAs;
        command.sourceSave = SourceSaveRequest{saved.Identity().instance, editedRevision, {}};
        command.stringPayload = "renamed.cpp";
        controller.ProcessCommand(command);
        REQUIRE(controller.SourceSaveOutcome()->HasValue());
        const auto relocated = controller.SourceSaveOutcome()->Value().snapshot;
        CHECK(relocated.Identity().instance == saved.Identity().instance);
        CHECK(relocated.Identity().key.source.Value() == "renamed.cpp");
        REQUIRE(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 1);
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().front().identity == relocated.Identity());
        command = {};
        command.command = EditorWorkspaceViewCommand::CloseSourceDocument;
        command.sourceSave = SourceSaveRequest{relocated.Identity().instance, relocated.Revision(), {}};
        command.sourceCloseDecision = SourceCloseDecision::Cancel;
        controller.ProcessCommand(command);
        REQUIRE(controller.SourceCloseOutcome());
        REQUIRE(controller.SourceCloseOutcome()->HasValue());
        CHECK_FALSE(controller.SourceCloseOutcome()->Value());
        CHECK(controller.SourceDocuments().Snapshot(relocated.Identity().instance).HasValue());
        command.sourceCloseDecision = SourceCloseDecision::Save;
        controller.ProcessCommand(command);
        REQUIRE(controller.SourceCloseOutcome()->HasValue());
        CHECK(controller.SourceCloseOutcome()->Value());
        CHECK(controller.SourceDocuments().Snapshot(relocated.Identity().instance).HasError());
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().empty());
        CHECK(relocated.Text() == "nextedited");
    }

    TEST_CASE("Workspace commands publish source saves and expose conflict and close outcomes", "[unit][editor][source][save]") {
        const auto projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-source-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(projectRoot);

        ScopedSourceProjectCleanup cleanup{projectRoot};

        {
            std::ofstream file(projectRoot / "code.cpp");
            file << "base";
        }
        std::optional<SourceDocumentSnapshot> opened;
        TestWorkspaceController controller{projectRoot, {}, [&](const SourceOpenResult &result) {
            opened = result.sourceSnapshot;
            return true;
        }};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenSourceFile;
        command.sourceOpenRequest = SourceOpenRequest{.path = "code.cpp"};
        controller.ProcessCommand(command);
        REQUIRE(opened);
        auto edited = controller.SourceDocuments().Edit(opened->Identity().instance, {opened->Revision(), 0, 4, "edited"});
        REQUIRE(edited.HasValue());
        command = {};
        command.command = EditorWorkspaceViewCommand::SaveSourceDocument;
        command.sourceSave = SourceSaveRequest{opened->Identity().instance, edited.Value().Revision(), {}};
        controller.ProcessCommand(command);
        REQUIRE(controller.SourceSaveOutcome());
        REQUIRE(controller.SourceSaveOutcome()->HasValue());
        CHECK_FALSE(controller.SourceSaveOutcome()->Value().snapshot.Dirty());
        const auto saved = controller.SourceSaveOutcome()->Value().snapshot;
        edited = controller.SourceDocuments().Edit(saved.Identity().instance, {saved.Revision(), 0, 0, "next"});
        REQUIRE(edited.HasValue());
        {
            std::ofstream file(projectRoot / "code.cpp");
            file << "external";
        }
        command = {};
        command.command = EditorWorkspaceViewCommand::SaveAllSourceDocuments;
        controller.ProcessCommand(command);
        REQUIRE(controller.SourceSaveAllOutcome());
        REQUIRE(controller.SourceSaveAllOutcome()->HasValue());
        REQUIRE(controller.SourceSaveAllOutcome()->Value().size() == 1);
        CHECK(controller.SourceSaveAllOutcome()->Value().front().result.HasError());
        CHECK(controller.SaveSourceDocuments().HasError());
        CheckSourceRelocationAndClose(controller, saved, edited.Value().Revision());
    }

    /** @brief Opens a source through the same embedded command used by the workspace document host. */
    void OpenWorkspaceSource(TestWorkspaceController &controller, const char *path) {
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenSourceFile;
        command.sourceOpenRequest = SourceOpenRequest{.path = path};
        controller.ProcessCommand(command);
    }

    /** @brief Exercises dirty generic close, stale discard, cancellation, and current-revision discard against canonical owners. */
    void CheckSourceCloseFences(TestWorkspaceController &controller, const SourceDocumentSnapshot &first,
                                const std::uint64_t editedRevision, const DocumentInstanceId secondInstance) {
        const auto &host = controller.ViewModel().workspacePanelHost;
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument;
        command.documentInstance = first.Identity().instance;
        controller.ProcessCommand(command);
        REQUIRE(controller.SourceCloseOutcome());
        CHECK(controller.SourceCloseOutcome()->HasError());
        CHECK(host.DocumentTabs().size() == 2);
        command.command = EditorWorkspaceViewCommand::CloseSourceDocument;
        command.sourceSave = SourceSaveRequest{first.Identity().instance, first.Revision(), {}};
        command.sourceCloseDecision = SourceCloseDecision::Discard;
        controller.ProcessCommand(command);
        CHECK(controller.SourceCloseOutcome()->HasError());
        CHECK(host.DocumentTabs().size() == 2);
        command.sourceSave->expectedRevision = editedRevision;
        command.sourceCloseDecision = SourceCloseDecision::Cancel;
        controller.ProcessCommand(command);
        CHECK_FALSE(controller.SourceCloseOutcome()->Value());
        CHECK(host.DocumentTabs().size() == 2);
        command.sourceCloseDecision = SourceCloseDecision::Discard;
        controller.ProcessCommand(command);
        CHECK(controller.SourceCloseOutcome()->Value());
        REQUIRE(host.DocumentTabs().size() == 1);
        CHECK(host.ActiveDocument() == secondInstance);
        CHECK_FALSE(controller.HasDirtySourceDocuments());
    }

    TEST_CASE("Workspace source tabs preserve independent owner state across focus reorder and fenced close",
              "[unit][editor][source][documents]") {
        const auto projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-source-tabs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(projectRoot);
        ScopedSourceProjectCleanup cleanup{projectRoot};
        for (const char *name : {"first.cpp", "second.cpp"}) {
            std::ofstream file(projectRoot / name);
            file << name;
        }
        TestWorkspaceController controller{projectRoot};
        OpenWorkspaceSource(controller, "first.cpp");
        OpenWorkspaceSource(controller, "second.cpp");
        const auto &host = controller.ViewModel().workspacePanelHost;
        REQUIRE(host.DocumentTabs().size() == 2);
        REQUIRE(controller.ViewModel().sourceDocuments.size() == 2);
        const auto first = controller.ViewModel().sourceDocuments[0];
        const auto second = controller.ViewModel().sourceDocuments[1];
        auto edited = controller.SourceDocuments().Edit(first.Identity().instance, {first.Revision(), 0, 0, "edited "});
        REQUIRE(edited.HasValue());
        OpenWorkspaceSource(controller, "first.cpp");
        CHECK(host.DocumentTabs().size() == 2);
        CHECK(host.ActiveDocument() == first.Identity().instance);
        CHECK(controller.SourceDocuments().Snapshot(first.Identity().instance).Value().Text() == "edited first.cpp");
        CHECK(controller.HasDirtySourceDocuments());
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::MoveWorkspaceDocument;
        command.documentInstance = second.Identity().instance;
        command.documentMoveBefore = first.Identity().instance;
        controller.ProcessCommand(command);
        CHECK(host.DocumentTabs()[0].identity == second.Identity());
        CHECK(controller.ViewModel().sourceDocuments[0].SharesStateWith(second));
        CHECK(controller.ViewModel().sourceDocuments[1].SharesStateWith(edited.Value()));
        CHECK(host.ActiveDocument() == first.Identity().instance);
        CheckSourceCloseFences(controller, first, edited.Value().Revision(), second.Identity().instance);
        OpenWorkspaceSource(controller, "first.cpp");
        REQUIRE(host.DocumentTabs().size() == 2);
        CHECK(host.DocumentTabs()[1].identity.instance != first.Identity().instance);
        CHECK(controller.ViewModel().sourceDocuments[1].Text() == "first.cpp");
        CHECK(first.Text() == "first.cpp");
        const auto reopened = controller.ViewModel().sourceDocuments[1];
        REQUIRE(controller.SourceDocuments().Edit(reopened.Identity().instance, {reopened.Revision(), 0, 0, "saved "}).HasValue());
        REQUIRE(controller.HasDirtySourceDocuments());
        REQUIRE(controller.SaveSourceDocuments().HasValue());
        CHECK_FALSE(controller.HasDirtySourceDocuments());
    }

    TEST_CASE("Source close modal routes dirty Save Discard and Cancel through the actual owner",
              "[unit][editor][gui][source][documents]") {
        const auto decision = GENERATE(SourceCloseDecision::Save, SourceCloseDecision::Discard, SourceCloseDecision::Cancel);
        Horo::Editor::Tests::EditorGuiContextFixture gui;
        const Horo::Tests::OwnedTestDirectory directory{"source-close-decisions"};
        {
            std::ofstream file(directory.Path() / "code.cpp");
            file << "base";
        }
        TestWorkspaceController controller{directory.Path()};
        EditorWorkspaceViewCommandData open;
        open.command = EditorWorkspaceViewCommand::OpenSourceFile;
        open.sourceOpenRequest = SourceOpenRequest{.path = "code.cpp"};
        controller.ProcessCommand(open);
        const auto source = controller.ViewModel().sourceDocuments.front();
        const auto edited = controller.SourceDocuments().Edit(source.Identity().instance, {source.Revision(), 0, 4, "edited"});
        REQUIRE(edited.HasValue());
        auto lifetime = std::make_shared<int>(0);
        SourceDocumentCloseModal modal{gui.context,
                                       controller.Controller(),
                                       lifetime,
                                       {source.Identity().instance, edited.Value().Revision(), {}},
                                       source.Identity().key.source.Value()};
        EditorModalHost host{gui.editorEvents, gui.input};
        EditorModalContext context{gui.editorEvents, host};
        REQUIRE(modal.OnOpen(context).HasValue());
        CHECK(modal.Id().IsValid());
        CHECK(modal.Presentation().size == ModalSizePolicy::Compact);
        CHECK(modal.ClosePolicy().allowEscape);
        CHECK(modal.CanClose(ModalCloseReason::Cancelled) == CloseDecision::Allow);
        const auto resolved = modal.Resolve(decision);
        REQUIRE(resolved.HasValue());
        CHECK(resolved.Value());
        CHECK(controller.SourceDocuments().Snapshot(source.Identity().instance).HasValue() == (decision == SourceCloseDecision::Cancel));
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().empty() == (decision != SourceCloseDecision::Cancel));
        std::ifstream disk(directory.Path() / "code.cpp");
        CHECK((std::string{std::istreambuf_iterator<char>{disk}, {}} == (decision == SourceCloseDecision::Save ? "edited" : "base")));
        CHECK(edited.Value().Text() == "edited");
    }

    TEST_CASE("Workspace canvas tabs share the revision-fenced close modal and recover after shutdown",
              "[unit][editor][gui][ui_canvas][recovery]") {
        using namespace Runtime::Ui;
        const Horo::Tests::OwnedTestDirectory project{"workspace-canvas-recovery"};
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        ProjectMutationCoordinator otherMutations(files);
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16, .ioWorkerCount = 1}};
        Editor::Tests::EditorGuiContextFixture gui;
        const auto path = project.Path() / "Hud.uicanvas";
        const auto owner = Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value();
        const auto nextOwner = Assets::AssetId::Parse("12345678-1234-4234-8234-123456789def").Value();
        SerializedUiId bytes{};
        bytes.back() = 1;
        const auto documentId = UiDocumentId::Create(bytes).Value();
        bytes.back() = 2;
        const auto canvasId = UiCanvasId::Create(bytes).Value();
        bytes.back() = 3;
        const auto rootId = UiElementId::Create(bytes).Value();
        const auto build = [&](std::uint64_t revision) {
            UiDocumentBuilder canvas(documentId, UiDocumentRevision::Create(revision).Value());
            REQUIRE(canvas.AddCanvas({.id = canvasId, .rootElement = rootId, .referenceResolution = {.width = 1920, .height = 1080}})
                        .HasValue());
            auto document = std::move(canvas).Build();
            REQUIRE(document.HasValue());
            return std::move(document).Value();
        };
        REQUIRE(SaveUiCanvasDocument(project.Path(), path, {.document = build(1), .state = {1}}, {}, false, mutations, files).HasValue());

        Runtime::RuntimeSceneService runtime;
        REQUIRE(runtime.Startup({}).HasValue());
        EditorWorkspaceController controller{project.Path(),
                                             runtime,
                                             {},
                                             {.world = {.documentSessionOwner = owner},
                                              .mutations = &mutations,
                                              .durableFiles = &files,
                                              .jobs = &jobs}};
        EditorWorkspaceViewCommandData open;
        open.command = EditorWorkspaceViewCommand::OpenSourceFile;
        open.sourceOpenRequest = SourceOpenRequest{.path = "Hud.uicanvas"};
        controller.ProcessCommand(open);
        REQUIRE(controller.ViewModel().uiCanvasDocuments.size() == 1);
        auto source = controller.ViewModel().uiCanvasDocuments.front();
        const auto firstState = source.state;
        EditorWorkspaceViewCommandData edit;
        edit.command = EditorWorkspaceViewCommand::UiCanvasCreate;
        edit.documentInstance = source.identity.instance;
        edit.uiCanvasState = source.state;
        edit.uiCanvasElement = source.root;
        controller.ProcessCommand(edit);
        source = controller.ViewModel().uiCanvasDocuments.front();
        REQUIRE(source.dirty);
        REQUIRE(source.canUndo);
        CHECK(source.state != firstState);

        const auto child = std::ranges::find_if(source.rows, [&](const auto &row) {
            return row.parent == source.root;
        });
        REQUIRE(child != source.rows.end());
        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::UiCanvasSelect;
        select.documentInstance = source.identity.instance;
        select.uiCanvasState = source.state;
        select.uiCanvasElement = child->element;
        controller.ProcessCommand(select);
        const auto &inspection = controller.ViewModel().uiCanvasDocuments.front().preview.selectedLayout;
        REQUIRE(inspection);
        CHECK(inspection->element == child->element);

        EditorWorkspaceViewCommandData close;
        close.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument;
        close.documentInstance = source.identity.instance;
        WorkspaceScreenDetail::WorkspaceSourceDocumentCommands routes;
        routes.BeginSession();
        REQUIRE(routes.HandleMenuAction(&controller, EditorMenuAction::Undo));
        const auto &undone = controller.ViewModel().uiCanvasDocuments.front();
        CHECK_FALSE(undone.dirty);
        CHECK_FALSE(undone.preview.selectedLayout);
        REQUIRE(routes.HandleMenuAction(&controller, EditorMenuAction::Redo));
        source = controller.ViewModel().uiCanvasDocuments.front();
        REQUIRE(source.dirty);
        EditorModalHost host{gui.editorEvents, gui.input};
        CHECK(routes.OpenCloseModal(controller, close, host, gui.context));
        host.OnUpdate(1.0F / 60.0F);
        CHECK(host.HasOpenModal());
        controller.ProcessCommand(close);
        REQUIRE(controller.ViewModel().uiCanvasDocuments.size() == 1);
        CHECK(controller.ViewModel().uiCanvasDocuments.front().dirty);

        auto lifetime = std::make_shared<int>(0);
        SourceDocumentCloseModal stale{gui.context, controller, lifetime, firstState, "Hud.uicanvas"};
        CHECK(stale.Resolve(SourceCloseDecision::Discard).HasError());
        SourceDocumentCloseModal choice{gui.context, controller, lifetime, source.state, "Hud.uicanvas"};
        CHECK(choice.Resolve(SourceCloseDecision::Cancel).Value());
        SECTION("external source changes keep dirty state open until explicit discard") {
            const auto external =
                SaveUiCanvasDocument(project.Path(), path, {.document = build(9), .state = {1}}, {}, true, otherMutations, files);
            REQUIRE(external.HasValue());
            CHECK(choice.Resolve(SourceCloseDecision::Save).HasError());
            CHECK(controller.ViewModel().uiCanvasDocuments.front().dirty);
            CHECK(choice.Resolve(SourceCloseDecision::Discard).Value());
            CHECK(controller.ViewModel().uiCanvasDocuments.empty());
            CHECK(LoadUiCanvasDocument(path).Value().document.Revision().Value() == 9);
        }
        SECTION("timed recovery uses the I/O worker and shutdown leaves a recoverable draft") {
            auto gate = std::make_shared<std::atomic_bool>(false);

            struct Unblock final {
                std::shared_ptr<std::atomic_bool> gate;

                ~Unblock() {
                    gate->store(true);
                }
            } unblock{gate};

            const auto occupied = jobs.SubmitResult({.resource = JobResource::Io}, [gate](const CancellationToken &cancellation) {
                while (!gate->load() && !cancellation.IsCancellationRequested())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return Result<void>::Success();
            });
            REQUIRE(occupied.HasValue());
            const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (occupied.Value().Snapshot()->state != JobState::Running && std::chrono::steady_clock::now() < startDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            REQUIRE(occupied.Value().Snapshot()->state == JobState::Running);
            controller.UpdateAutosave(60, 1);
            DocumentRecoveryService storage(project.Path(), mutations, files);
            const auto pending = storage.Inspect(Assets::AssetId::FromBytes(documentId.Bytes()), source.identity.key);
            REQUIRE(pending.HasValue());
            CHECK_FALSE(pending.Value());
            CHECK(jobs.AdmissionSnapshot().queued[static_cast<std::size_t>(JobPriority::Background)] == 1);
            SECTION("explicit save cancels accepted work before canonical publication") {
                REQUIRE(controller.SaveSourceDocuments().HasValue());
                CHECK_FALSE(controller.HasDirtySourceDocuments());
                CHECK(jobs.AdmissionSnapshot().queued[static_cast<std::size_t>(JobPriority::Background)] == 0);
                CHECK_FALSE(storage.Inspect(Assets::AssetId::FromBytes(documentId.Bytes()), source.identity.key).Value());
                CHECK(LoadUiCanvasDocument(path).Value().document.Revision() == source.documentRevision);
            }
            SECTION("completed recovery restores in a newly opened workspace") {
                gate->store(true);

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                bool durable{};
                while (!durable && std::chrono::steady_clock::now() < deadline) {
                    controller.UpdateAutosave(0, 1);
                    const auto checkpoint = storage.Inspect(Assets::AssetId::FromBytes(documentId.Bytes()), source.identity.key);
                    durable = checkpoint.HasValue() && checkpoint.Value().has_value();
                    if (!durable)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                REQUIRE(durable);
                REQUIRE(controller.ShutdownUiCanvasDocuments().HasValue());
                CHECK(controller.ViewModel().uiCanvasDocuments.empty());
                CHECK(LoadUiCanvasDocument(path).Value().document.Revision().Value() == 1);
                EditorWorkspaceController reopened{project.Path(),
                                                   runtime,
                                                   {},
                                                   {.world = {.documentSessionOwner = nextOwner},
                                                    .mutations = &mutations,
                                                    .durableFiles = &files}};
                reopened.ProcessCommand(open);
                const auto &canvas = reopened.ViewModel().uiCanvasDocuments.front();
                REQUIRE(canvas.recovery);
                EditorWorkspaceViewCommandData restore;
                restore.command = EditorWorkspaceViewCommand::UiCanvasRecover;
                restore.documentInstance = canvas.identity.instance;
                restore.uiCanvasState = canvas.state;
                restore.uiCanvasRecovery = canvas.recovery;
                reopened.ProcessCommand(restore);
                REQUIRE(reopened.ViewModel().uiCanvasDocuments.front().dirty);
                CHECK(reopened.ViewModel().uiCanvasDocuments.front().documentRevision == source.documentRevision);
                REQUIRE(reopened.SaveSourceDocuments().HasValue());
                CHECK_FALSE(reopened.HasDirtySourceDocuments());
                CHECK(LoadUiCanvasDocument(path).Value().document.Revision() == source.documentRevision);
            }
        }
    }

    TEST_CASE("Source close modal retains stale conflicted and expired documents and renders recovery",
              "[unit][editor][gui][source][documents]") {
        const int failure = GENERATE(0, 1, 2);
        const float width = GENERATE(1280.0F, 320.0F);
        Horo::Editor::Tests::EditorGuiContextFixture gui{{width, 600}};
        REQUIRE(gui.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/tr-TR.json"));
        REQUIRE(gui.localization.Prepare(LocaleTag{"tr-TR"}));
        REQUIRE(gui.localization.ActivatePrepared());
        const Horo::Tests::OwnedTestDirectory directory{"source-close-recovery"};
        {
            std::ofstream file(directory.Path() / "code.cpp");
            file << "base";
        }
        TestWorkspaceController controller{directory.Path()};
        EditorWorkspaceViewCommandData open;
        open.command = EditorWorkspaceViewCommand::OpenSourceFile;
        open.sourceOpenRequest = SourceOpenRequest{.path = "code.cpp"};
        controller.ProcessCommand(open);
        const auto source = controller.ViewModel().sourceDocuments.front();
        const auto edited = controller.SourceDocuments().Edit(source.Identity().instance, {source.Revision(), 0, 4, "edited"});
        REQUIRE(edited.HasValue());
        auto lifetime = std::make_shared<int>(0);
        auto modal =
            std::make_unique<SourceDocumentCloseModal>(gui.context, controller.Controller(), lifetime,
                                                       SourceSaveRequest{source.Identity().instance, edited.Value().Revision(), {}},
                                                       std::string(256, 'x') + "/code.cpp");
        auto *surface = modal.get();
        EditorModalHost host{gui.editorEvents, gui.input};
        REQUIRE(host.OpenRoot(std::move(modal)).HasValue());
        host.OnUpdate(1.0F / 60.0F);
        gui.imgui.BeginFrame();
        host.Draw();
        gui.imgui.EndFrame();
        if (failure == 0)
            REQUIRE(controller.SourceDocuments().Edit(source.Identity().instance, {edited.Value().Revision(), 0, 0, "newer "}).HasValue());
        else if (failure == 1) {
            std::ofstream file(directory.Path() / "code.cpp");
            file << "external";
        } else
            lifetime.reset();
        const auto rejected = surface->Resolve(SourceCloseDecision::Save);
        REQUIRE(rejected.HasError());
        REQUIRE(controller.SourceDocuments().Snapshot(source.Identity().instance).HasValue());
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 1);
        gui.imgui.BeginFrame();
        host.Draw();
        gui.imgui.EndFrame();
        CHECK(host.HasOpenModal());
        CHECK(surface->Resolve(SourceCloseDecision::Cancel).Value());
    }

    TEST_CASE("Unavailable source owners clear retained content after tab reorder", "[unit][editor][source][documents]") {
        const Horo::Tests::OwnedTestDirectory directory{"workspace-source-unavailable"};
        for (const auto *name : {"first.cpp", "second.cpp"}) {
            std::ofstream file(directory.Path() / name);
            file << name;
        }
        TestWorkspaceController controller{directory.Path()};
        for (const auto *name : {"first.cpp", "second.cpp"}) {
            EditorWorkspaceViewCommandData open;
            open.command = EditorWorkspaceViewCommand::OpenSourceFile;
            open.sourceOpenRequest = SourceOpenRequest{.path = name};
            controller.ProcessCommand(open);
        }
        REQUIRE(controller.ViewModel().sourceDocuments.size() == 2);
        const auto first = controller.ViewModel().sourceDocuments[0];
        const auto second = controller.ViewModel().sourceDocuments[1];
        const auto edited = controller.SourceDocuments().Edit(second.Identity().instance, {second.Revision(), 0, 0, "edited "});
        REQUIRE(edited.HasValue());
        REQUIRE(controller.HasDirtySourceDocuments());
        REQUIRE(controller.SourceDocuments().Close(first.Identity().instance).HasValue());
        EditorWorkspaceViewCommandData move;
        move.command = EditorWorkspaceViewCommand::MoveWorkspaceDocument;
        move.documentInstance = second.Identity().instance;
        move.documentMoveBefore = first.Identity().instance;
        controller.ProcessCommand(move);
        const auto &model = controller.ViewModel();
        REQUIRE(model.sourceDocuments.size() == 2);
        CHECK(model.sourceDocuments[0].SharesStateWith(edited.Value()));
        CHECK(model.sourceDocuments[1].Revision() == 0);
        CHECK_FALSE(model.sourceDocuments[1].SharesStateWith(first));
        REQUIRE(model.workspacePanelHost.DocumentTabs().size() == 2);
        CHECK(model.workspacePanelHost.DocumentTabs()[0].dirty);
        CHECK_FALSE(model.workspacePanelHost.DocumentTabs()[1].dirty);
        controller.Controller().UpdateGameplaySources(0.0F);
        CHECK(model.sourceDocuments[1].Revision() == 0);
        REQUIRE(controller.SaveSourceDocuments().HasValue());
        EditorWorkspaceViewCommandData close;
        close.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument;
        close.documentInstance = second.Identity().instance;
        controller.ProcessCommand(close);
        REQUIRE(model.workspacePanelHost.DocumentTabs().size() == 1);
        CHECK(model.workspacePanelHost.DocumentTabs().front().identity == first.Identity());
        REQUIRE(model.sourceDocuments.size() == 1);
        CHECK(model.sourceDocuments.front().Revision() == 0);
    }

    TEST_CASE("Unchanged workspace source frames retain publications without allocation", "[unit][editor][source][documents][allocation]") {
        const Horo::Tests::OwnedTestDirectory directory{"workspace-source-unchanged"};
        std::filesystem::create_directory(directory.Path() / "source");
        constexpr const char *key = "source/large_source_document_for_unchanged_frame_qualification.cpp";
        {
            std::ofstream file(directory.Path() / key);
            file << std::string(1'048'576, 'a');
        }
        TestWorkspaceController controller{directory.Path()};
        EditorWorkspaceViewCommandData open;
        open.command = EditorWorkspaceViewCommand::OpenSourceFile;
        open.sourceOpenRequest = SourceOpenRequest{.path = key};
        controller.ProcessCommand(open);
        const auto source = controller.ViewModel().sourceDocuments.front();
        const auto edited = controller.SourceDocuments().Edit(source.Identity().instance, {source.Revision(), 1'048'575, 1, "b"});
        REQUIRE(edited.HasValue());
        REQUIRE(controller.HasDirtySourceDocuments());
        for (int frame = 0; frame < 8; ++frame)
            controller.Controller().UpdateGameplaySources(0.0F);
        Horo::Tests::AllocationProbe::Measurement observed;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement measure;
            for (int frame = 0; frame < 32; ++frame)
                controller.Controller().UpdateGameplaySources(0.0F);
            observed = measure.Snapshot();
        }
        CHECK(observed.requests == 0);
        CHECK(controller.ViewModel().sourceDocuments.front().SharesStateWith(edited.Value()));
        const auto reloaded = controller.SourceDocuments().Reload(source.Identity().instance, edited.Value().Revision());
        REQUIRE(reloaded.HasValue());
        controller.Controller().UpdateGameplaySources(0.0F);
        CHECK(controller.ViewModel().sourceDocuments.front().SharesStateWith(reloaded.Value()));
        CHECK_FALSE(controller.HasDirtySourceDocuments());
    }

    TEST_CASE("Source close footer uses one display scale for all choices", "[unit][editor][gui][source][documents]") {
        auto baseline = DesignSystem::DefaultDesignTokens();
        const float logical = SourceDocumentCloseModal::LogicalFooterHeight(baseline);
        DesignSystem::ApplyGlobalUiScale(baseline, 2.0F);
        CHECK(SourceDocumentCloseModal::LogicalFooterHeight(baseline) == Catch::Approx(logical));
    }

    /** @brief Owns the source history scenario's filesystem for the controller lifetime. */
    struct SourceHistoryProject final {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-source-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ScopedSourceProjectCleanup cleanup{root};

        SourceHistoryProject() {
            std::filesystem::create_directory(root);
            std::ofstream file(root / "code.cpp");
            file << "base";
            REQUIRE(file.good());
        }
    };

    TEST_CASE("Workspace source interactions publish exact revisions history and view restoration", "[unit][editor][source][history]") {
        SourceHistoryProject project;
        TestWorkspaceController controller{project.root, {}, [](const SourceOpenResult &) {
            return true;
        }};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenSourceFile;
        command.sourceOpenRequest = SourceOpenRequest{.path = "code.cpp"};
        controller.ProcessCommand(command);
        REQUIRE(controller.ViewModel().sourceDocuments.size() == 1);
        const auto snapshot = controller.ViewModel().sourceDocuments.front();
        const auto id = snapshot.Identity().instance;
        const SourceEditingView view{0, 4, -5, SourceEditingViewId{77}};
        const auto plan = PlanSourceReplacement(snapshot, view, "edited");
        REQUIRE(plan.HasValue());
        command = {};
        command.command = EditorWorkspaceViewCommand::SourceDocumentInteraction;
        command.sourceInteraction = SourceEditSubmission{id, plan.Value()};
        controller.ProcessCommand(command);
        REQUIRE(controller.ViewModel().sourceEditingResult);
        REQUIRE(controller.ViewModel().sourceEditingResult->HasValue());
        CHECK(controller.ViewModel().sourceDocuments.front().Text() == "edited");
        CHECK(controller.ViewModel().sourceHistory.front().undoEntries == 1);
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().front().dirty);
        command.sourceInteraction = SourceHistoryRequest{id, 2, SourceHistoryDirection::Undo};
        controller.ProcessCommand(command);
        REQUIRE(controller.ViewModel().sourceEditingResult->HasValue());
        CHECK(controller.ViewModel().sourceEditingResult->Value().restoration == view);
        CHECK(controller.ViewModel().sourceDocuments.front().Text() == "base");
        CHECK_FALSE(controller.ViewModel().workspacePanelHost.DocumentTabs().front().dirty);
        command.sourceInteraction = SourceHistoryRequest{id, 2, SourceHistoryDirection::Redo};
        controller.ProcessCommand(command);
        REQUIRE(controller.ViewModel().sourceEditingResult->HasError());
        CHECK(controller.ViewModel().sourceDocuments.front().Text() == "base");
        CHECK(controller.ViewModel().sourceHistory.front().redoEntries == 1);
        command.sourceInteraction = SourceHistoryRequest{id, 3, SourceHistoryDirection::Redo};
        controller.ProcessCommand(command);
        REQUIRE(controller.ViewModel().sourceEditingResult->HasValue());
        CHECK(controller.ViewModel().sourceDocuments.front().Text() == "edited");
        command.sourceInteraction.reset();
        controller.ProcessCommand(command);
        REQUIRE(controller.ViewModel().sourceEditingResult->HasError());
        CHECK(controller.ViewModel().sourceDocuments.front().Revision() == 4);
    }
}  // namespace
