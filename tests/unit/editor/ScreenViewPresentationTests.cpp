#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "editor/screens/project_loading/ProjectLoadingView.h"
#include "editor/screens/welcome/WelcomeView.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
    Horo::Editor::RecentProjectEntry MakeRecentProject(const Horo::Application::ProjectCompatibilityStatus status,
                                                       const Horo::Editor::RecentProjectInspectionState inspectionState) {
        using namespace Horo::Editor;
        return RecentProjectEntry{
            .name = "Example Project",
            .rootPath = "/projects/example",
            .lastOpenedLabel = "Yesterday",
            .thumbnailKey = "example",
            .compatibility =
                RecentProjectCompatibilityProjection{
                    .status = status,
                    .inspectionState = inspectionState,
                },
        };
    }

    class RecordingWorkspacePanel final : public Horo::Editor::IWorkspacePanel {
    public:
        RecordingWorkspacePanel(std::string id, const Horo::Editor::WorkspaceDockArea area) : id_{std::move(id)}, area_{area} {}

        [[nodiscard]] std::string GetId() const override {
            return id_;
        }

        [[nodiscard]] std::string GetDisplayName() const override {
            return id_ + ".title";
        }

        [[nodiscard]] Horo::Editor::WorkspaceDockArea GetDefaultDockArea() const override {
            return area_;
        }

        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override {
            return {};
        }

        void OnAttach(Horo::Editor::PanelContext &) override {}

        void OnDetach() override {}

        void DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, const ImU32 color) override {
            drawList->AddRect(position, {position.x + size.x, position.y + size.y}, color);
        }

        void DrawPanel(const ImVec2 &, const ImVec2 &, const Horo::Editor::EditorWorkspaceViewModel &,
                       Horo::Editor::EditorWorkspaceViewCommandData &, const Horo::Editor::EditorGuiContext &) override {
            ++drawCount;
        }

        int drawCount{};

    private:
        std::string id_;
        Horo::Editor::WorkspaceDockArea area_;
    };
}  // namespace

TEST_CASE("Welcome presentation renders recent-project compatibility states and news", "[unit][editor][gui][welcome]") {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Editor;

    Tests::EditorGuiContextFixture fixture;
    static constexpr std::array statuses{
        ProjectCompatibilityStatus::Current,
        ProjectCompatibilityStatus::CompatibleReleaseLine,
        ProjectCompatibilityStatus::AutomaticMigrationRequired,
        ProjectCompatibilityStatus::RecoveryRequired,
        ProjectCompatibilityStatus::FutureVersion,
        ProjectCompatibilityStatus::MigrationPathMissing,
        ProjectCompatibilityStatus::RequiredProviderUnavailable,
        ProjectCompatibilityStatus::Corrupt,
        ProjectCompatibilityStatus::Inaccessible,
    };

    WelcomeViewModel model{
        .productName = "Horo",
        .statusLabel = "Ready",
        .whatsNew = {WhatsNewEntry{"Release", "Editor workflows", "Improved editor workflows."},
                     WhatsNewEntry{"Docs", "Architecture", "Updated architecture guidance."}},
    };
    for (std::size_t index = 0; index < statuses.size(); ++index) {
        model.recentProjects.push_back(MakeRecentProject(statuses[index], index == 0 ? RecentProjectInspectionState::Refreshing
                                                                                     : RecentProjectInspectionState::Fresh));
    }
    model.recentProjects.push_back(RecentProjectEntry{"Legacy Project", "/projects/legacy", "Last week", "legacy", std::nullopt});
    WelcomeViewState state;

    fixture.imgui.BeginFrame();
    const WelcomeViewResult result =
        DrawWelcomeView(model, state, fixture.context, WelcomeViewAssets{}, GuiContentRegion{0.0F, 0.0F, 1280.0F, 800.0F});
    fixture.imgui.EndFrame();

    REQUIRE(result.command == WelcomeViewCommand::None);
    REQUIRE(result.openRecentIndex == -1);
    REQUIRE(model.recentProjects.size() == statuses.size() + 1);
}

TEST_CASE("Project loading presentation renders active cancelled and failure states", "[unit][editor][gui][project-loading]") {
    using namespace Horo::Editor;

    Tests::EditorGuiContextFixture fixture;
    const GuiContentRegion content{0.0F, 0.0F, 1280.0F, 800.0F};
    std::array states{
        ProjectLoadingViewState{.projectName = "Active", .projectRoot = "/projects/active", .progress = 42.0F},
        ProjectLoadingViewState{.projectName = "Cancelled", .projectRoot = "/projects/cancelled", .progress = 60.0F, .isCancelled = true},
        ProjectLoadingViewState{.projectName = "Retryable",
                                .projectRoot = "/projects/retryable",
                                .progress = 75.0F,
                                .statusText = "Import failed",
                                .hasFailed = true,
                                .canRetry = true},
        ProjectLoadingViewState{.projectName = "Failed",
                                .projectRoot = "/projects/failed",
                                .progress = 100.0F,
                                .statusText = "Project cannot be opened",
                                .hasFailed = true},
    };

    for (auto &state : states) {
        fixture.imgui.BeginFrame();
        const ProjectLoadingViewCommand command = DrawProjectLoadingView(state, fixture.context, content);
        fixture.imgui.EndFrame();
        REQUIRE(command == ProjectLoadingViewCommand::None);
    }
}

TEST_CASE("Workspace presentation renders lifecycle and recovery states without panels", "[unit][editor][gui][workspace]") {
    using namespace Horo;
    using namespace Horo::Editor;

    Tests::EditorGuiContextFixture fixture;
    WorkspacePanelRegistry panels;
    auto workspaceInput = fixture.input.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
    EditorWorkspaceView view{fixture.context, panels, 0, fixture.input, workspaceInput};
    EditorWorkspaceViewModel model;
    model.projectRoot = "/projects/example";
    model.fps = 60.0F;
    model.canUndo = true;
    model.canRedo = true;
    const GuiContentRegion content{0.0F, 0.0F, 1280.0F, 800.0F};

    static constexpr std::array playStates{
        EditorPlayState::Idle,   EditorPlayState::Starting, EditorPlayState::Playing,
        EditorPlayState::Paused, EditorPlayState::Stopping, EditorPlayState::Failed,
    };
    for (const EditorPlayState playState : playStates) {
        model.playState = playState;
        model.playError = playState == EditorPlayState::Failed ? "Runtime launch failed" : "";
        model.recoveryAvailable = playState == EditorPlayState::Starting;
        model.sceneExternalConflict = playState == EditorPlayState::Paused;
        EditorWorkspaceViewCommandData command;

        fixture.imgui.BeginFrame();
        view.Draw(model, command, content);
        fixture.imgui.EndFrame();

        REQUIRE(command.command == EditorWorkspaceViewCommand::None);
    }
}

TEST_CASE("Workspace presentation routes registered panels through every dock area", "[unit][editor][gui][workspace]") {
    using namespace Horo;
    using namespace Horo::Editor;

    Tests::EditorGuiContextFixture fixture;
    WorkspacePanelRegistry panels;
    const auto left = std::make_shared<RecordingWorkspacePanel>("test.left", WorkspaceDockArea::Left);
    const auto right = std::make_shared<RecordingWorkspacePanel>("test.right", WorkspaceDockArea::Right);
    const auto bottom = std::make_shared<RecordingWorkspacePanel>("test.bottom", WorkspaceDockArea::Bottom);
    const auto document = std::make_shared<RecordingWorkspacePanel>("test.document", WorkspaceDockArea::Document);
    panels.RegisterPanel(left);
    panels.RegisterPanel(right);
    panels.RegisterPanel(bottom);
    panels.RegisterPanel(document);

    auto workspaceInput = fixture.input.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
    EditorWorkspaceView view{fixture.context, panels, 0, fixture.input, workspaceInput};
    EditorWorkspaceViewModel model;
    model.activeLeftPanelId = left->GetId();
    model.activeRightPanelId = right->GetId();
    model.activeBottomPanelId = bottom->GetId();
    model.activeDocumentPanelId = document->GetId();
    REQUIRE(model.activityBarLayout.Insert(left->GetId(), {ActivityBarRail::Left, 0, 0}).Succeeded());
    REQUIRE(model.activityBarLayout.Insert(right->GetId(), {ActivityBarRail::Right, 0, 0}).Succeeded());
    REQUIRE(model.activityBarLayout.Insert(bottom->GetId(), {ActivityBarRail::Bottom, 0, 0}).Succeeded());
    REQUIRE(model.activityBarLayout.Insert(document->GetId(), {ActivityBarRail::DocumentTop, 0, 0}).Succeeded());

    EditorWorkspaceViewCommandData command;
    fixture.imgui.BeginFrame();
    view.Draw(model, command, GuiContentRegion{0.0F, 0.0F, 1280.0F, 800.0F});
    fixture.imgui.EndFrame();

    REQUIRE(command.command == EditorWorkspaceViewCommand::None);
    REQUIRE(left->drawCount == 1);
    REQUIRE(right->drawCount == 1);
    REQUIRE(bottom->drawCount == 1);
    REQUIRE(document->drawCount == 1);
}
