#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
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
        command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = outsideSource.string(), .line = 1};
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.global_dock.build_output.source.invalid"));

        command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = (projectRoot / "source/Missing.cpp").string(), .line = 3};
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.source_open.missing"));

        const std::filesystem::path escapingLink = projectRoot / "source/escape.cpp";
        std::error_code symlinkError;
        std::filesystem::create_symlink(outsideSource, escapingLink, symlinkError);
        if (!symlinkError) {
            command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = escapingLink.string(), .line = 4};
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
        const std::filesystem::path sourcePath = projectRoot / "assets/shaders/material.glsl";
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
        command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = sourcePath.string(), .line = 27, .column = 9};
        controller.ProcessCommand(command);

        REQUIRE(navigated.has_value());
        REQUIRE((std::filesystem::weakly_canonical(std::filesystem::path{navigated->absolutePath}) ==
                 std::filesystem::weakly_canonical(sourcePath)));
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
        CheckSourceRelocationAndClose(controller, saved, edited.Value().Revision());
    }

}  // namespace
