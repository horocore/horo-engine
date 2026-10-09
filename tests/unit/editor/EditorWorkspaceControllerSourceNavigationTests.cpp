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

}  // namespace
