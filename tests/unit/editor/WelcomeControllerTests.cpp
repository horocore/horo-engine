#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/WelcomeController.h"
#include "support/editor/ScopedTestHome.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {
    TEST_CASE("Route payload validation rejects mismatched parameters", "[unit][editor][welcome]") {
        using namespace Horo::Editor;

        REQUIRE((IsRoutePayloadValid(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}})));
        REQUIRE((IsRoutePayloadValid(GuiRoute{GuiRouteKind::ProjectBrowser, ProjectBrowserRouteParameters{}})));
        REQUIRE((IsRoutePayloadValid(GuiRoute{GuiRouteKind::ProjectCreation, ProjectCreationRouteParameters{}})));
        REQUIRE((IsRoutePayloadValid(
            GuiRoute{GuiRouteKind::EditorWorkspace, EditorWorkspaceRouteParameters{ProjectSessionCandidateId{1}, std::nullopt}})));
        REQUIRE((!IsRoutePayloadValid(
            GuiRoute{GuiRouteKind::EditorWorkspace, EditorWorkspaceRouteParameters{ProjectSessionCandidateId{}, std::nullopt}})));

        REQUIRE((!IsRoutePayloadValid(GuiRoute{GuiRouteKind::Welcome, ProjectBrowserRouteParameters{}})));
    }

    TEST_CASE("Welcome filters invalid recent projects", "[unit][editor][welcome]") {
        using namespace Horo::Editor;

        const std::string validRoot = (std::filesystem::temp_directory_path() / "horo-valid-project").string();

        WelcomeScreenController controller{{
            RecentProjectEntry{"Valid", validRoot, "today", "valid"},
            RecentProjectEntry{"Missing Path", "", "today", "missing"},
            RecentProjectEntry{"", "/tmp/missing-name", "today", "missing-name"},
            RecentProjectEntry{"Relative Path", "~/projects/example", "today", "relative"},
        }};

        const WelcomeViewModel model = controller.BuildViewModel();
        REQUIRE((model.productName == "Horo Editor"));
        REQUIRE((model.recentProjects.size() == 1));
        REQUIRE((model.recentProjects[0].name == "Valid"));

        const std::optional<WelcomeAction> openRecent = controller.RequestOpenRecentProject(0);
        REQUIRE((openRecent.has_value()));
        REQUIRE((openRecent->kind == WelcomeActionKind::OpenRecentProject));
        REQUIRE((openRecent->route.kind == GuiRouteKind::ProjectLoading));

        const auto *parameters = std::get_if<ProjectLoadingRouteParameters>(&openRecent->route.parameters);
        REQUIRE((parameters != nullptr));
        REQUIRE((parameters->projectRoot == validRoot));

        REQUIRE((!controller.RequestOpenRecentProject(1).has_value()));
    }

    TEST_CASE("Welcome preview text is deterministic", "[unit][editor][welcome]") {
        using namespace Horo::Editor;

        const std::string projectRoot = (std::filesystem::temp_directory_path() / "horo-preview-project").string();
        WelcomeScreenController controller{{RecentProjectEntry{"Project", projectRoot, "today", "project"}}};
        const WelcomeViewModel viewModel = controller.BuildViewModel();
        const std::string text = RenderWelcomeScreenText(viewModel);

        REQUIRE((text.find("Horo Editor") != std::string::npos));
        REQUIRE((text.find(viewModel.statusLabel) != std::string::npos));
        REQUIRE((text.find("Project") != std::string::npos));
        REQUIRE((text.find(projectRoot) != std::string::npos));
    }

    TEST_CASE("Cached compatibility projection round trips", "[unit][editor][welcome]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-welcome-controller"};
        using namespace Horo::Application;
        using namespace Horo::Editor;

        const auto current = CurrentEngineReleaseVersion();
        const std::string root = (std::filesystem::temp_directory_path() / "horo-cached-project").string();
        RecentProjectEntry entry{"Cached", root, "today", "custom"};
        entry.compatibility = RecentProjectCompatibilityProjection{.projectVersion = current,
                                                                   .status = ProjectCompatibilityStatus::Current,
                                                                   .targetVersion = current,
                                                                   .inspectionState = RecentProjectInspectionState::Fresh};
        REQUIRE((SaveRecentProjectsToDisk({entry})));
        const auto loaded = LoadRecentProjectsFromDisk();
        REQUIRE((loaded.size() == 1));
        REQUIRE((loaded.front().compatibility.has_value()));
        REQUIRE((loaded.front().compatibility->status == ProjectCompatibilityStatus::Current));
        REQUIRE((loaded.front().compatibility->projectVersion == current));
        REQUIRE((loaded.front().compatibility->inspectionState == RecentProjectInspectionState::Cached));
    }

    TEST_CASE("Removing the last recent project persists an empty list", "[unit][editor][welcome]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-welcome-empty-list"};
        REQUIRE(Horo::Editor::SaveRecentProjectsToDisk({}));
        REQUIRE(Horo::Editor::LoadRecentProjectsFromDisk().empty());
    }

    TEST_CASE("Project deletion requires a project marker", "[unit][editor][welcome]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-welcome-delete"};
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-welcome-delete-project-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(std::filesystem::create_directories(root / ".horo"));
        REQUIRE_FALSE(Horo::Editor::DeleteRecentProjectFiles(root));
        REQUIRE(std::filesystem::exists(root));

        std::ofstream(root / ".horo/project.json") << "{}";
        REQUIRE(Horo::Editor::DeleteRecentProjectFiles(root));
        REQUIRE_FALSE(std::filesystem::exists(root));
    }

    TEST_CASE("Project deletion rejects the user home even with a project marker", "[unit][editor][welcome]") {
        const Horo::TestSupport::ScopedTestHome home{"horo-welcome-protected-home"};
        const std::filesystem::path root = Horo::Editor::ResolveEditorSettingsPath().parent_path().parent_path();
        REQUIRE(std::filesystem::create_directories(root / ".horo"));
        std::ofstream(root / ".horo/project.json") << "{}";
        REQUIRE_FALSE(Horo::Editor::DeleteRecentProjectFiles(root));
        REQUIRE(std::filesystem::exists(root / ".horo/project.json"));
    }
}  // namespace
