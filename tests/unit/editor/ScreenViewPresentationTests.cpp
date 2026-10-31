#include "../../support/OwnedTestDirectory.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "editor/screens/LeaveConfirmationView.h"
#include "editor/screens/project_loading/ProjectLoadingView.h"
#include "editor/screens/welcome/WelcomeView.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <imgui_internal.h>
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
        RecordingWorkspacePanel(std::string id, const Horo::Editor::WorkspaceDockArea area)
            : IWorkspacePanel("test.RecordingWorkspacePanel"), id_{std::move(id)}, area_{area} {}

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

        void DrawPanel(const ImVec2 &, const ImVec2 &size, const Horo::Editor::EditorWorkspaceViewModel &,
                       Horo::Editor::EditorWorkspaceViewCommandData &, const Horo::Editor::EditorGuiContext &) override {
            ++drawCount;
            lastSize = size;
        }

        int drawCount{};
        ImVec2 lastSize{};

    private:
        std::string id_;
        Horo::Editor::WorkspaceDockArea area_;
    };

    /** @brief Drives the rendered Save action and unavailable-owner presentation using the same workspace view. */
    void CheckSourceSaveAndUnavailable(Horo::Editor::Tests::EditorGuiContextFixture &fixture, Horo::Editor::EditorWorkspaceView &view,
                                       Horo::Editor::EditorWorkspaceViewModel &model, const Horo::Editor::SourceDocumentSnapshot &source,
                                       const float width) {
        using namespace Horo::Editor;
        ImGuiID saveId{};
        for (auto *const window : ImGui::GetCurrentContext()->Windows) {
            const std::string_view name{window->Name};
            constexpr std::string_view content{"##DockDocument/##DockContent"};
            const auto position = name.find(content);
            // Descendant widget windows inherit this prefix but own a different ID stack.
            if (position != std::string_view::npos && name.find('/', position + content.size()) == std::string_view::npos) {
                REQUIRE(saveId == 0);
                saveId = window->GetID(fixture.localization.Get("editor", "workspace.documents.save").c_str());
            }
        }
        REQUIRE(saveId != 0);
        ImGui::ActivateItemByID(saveId);
        EditorWorkspaceViewCommandData save;
        fixture.imgui.BeginFrame();
        view.Draw(model, save, GuiContentRegion{0, 0, width, 600});
        fixture.imgui.EndFrame();
        REQUIRE(save.command == EditorWorkspaceViewCommand::SaveSourceDocument);
        REQUIRE(save.sourceSave);
        CHECK(save.sourceSave->instance == source.Identity().instance);
        CHECK(save.sourceSave->expectedRevision == source.Revision());
        // A surviving host identity must show an unavailable owner rather than another tab's cached text.
        REQUIRE(model.workspacePanelHost.FocusDocument(model.workspacePanelHost.DocumentTabs()[1].identity.instance).HasValue());
        EditorWorkspaceViewCommandData unavailable;
        fixture.imgui.BeginFrame();
        view.Draw(model, unavailable, GuiContentRegion{0, 0, width, 600});
        fixture.imgui.EndFrame();
        CHECK(unavailable.command == EditorWorkspaceViewCommand::None);
    }
}  // namespace

TEST_CASE("Welcome presentation renders recent-project compatibility states and news", "[unit][editor][gui][welcome]") {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Editor;

    Horo::Editor::Tests::EditorGuiContextFixture fixture;
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

    Horo::Editor::Tests::EditorGuiContextFixture fixture;
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
        const auto *fallback = ImGui::FindWindowByName("Debug##Default");
        REQUIRE((fallback == nullptr || !fallback->Active));
    }
}

TEST_CASE("Workspace presentation renders lifecycle and recovery states without panels", "[unit][editor][gui][workspace]") {
    using namespace Horo;
    using namespace Horo::Editor;

    Horo::Editor::Tests::EditorGuiContextFixture fixture;
    WorkspacePanelRegistry panels;
    auto workspaceInput = fixture.input.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
    EditorWorkspaceView view{fixture.context, panels, 0, fixture.input, workspaceInput};
    EditorWorkspaceViewModel model;
    model.projectRoot = "/projects/example";
    model.projectName = "Very long localized project name — Uzun Proje Adı";
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

    Horo::Editor::Tests::EditorGuiContextFixture fixture;
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

TEST_CASE("Workspace presentation clamps oversized docks at narrow bounds", "[unit][editor][gui][workspace]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Horo::Editor::Tests::EditorGuiContextFixture fixture;
    WorkspacePanelRegistry panels;
    const auto left = std::make_shared<RecordingWorkspacePanel>("test.left", WorkspaceDockArea::Left);
    const auto right = std::make_shared<RecordingWorkspacePanel>("test.right", WorkspaceDockArea::Right);
    const auto bottom = std::make_shared<RecordingWorkspacePanel>("test.bottom", WorkspaceDockArea::Bottom);
    panels.RegisterPanel(left);
    panels.RegisterPanel(right);
    panels.RegisterPanel(bottom);
    auto input = fixture.input.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
    EditorWorkspaceView view{fixture.context, panels, 0, fixture.input, input};
    EditorWorkspaceViewModel model;
    model.activeLeftPanelId = model.activeLeftTopPanelId = model.activeLeftBottomPanelId = left->GetId();
    model.activeRightPanelId = model.activeRightTopPanelId = model.activeRightBottomPanelId = right->GetId();
    model.activeBottomPanelId = model.activeBottomLeftPanelId = model.activeBottomRightPanelId = bottom->GetId();
    model.leftPanelWidth = model.rightPanelWidth = model.bottomPanelHeight = 10000.0F;
    model.leftDockMode = model.rightDockMode = GENERATE(SideDockMode::Full, SideDockMode::Split);
    model.bottomDockMode = GENERATE(BottomDockMode::Full, BottomDockMode::Split);
    const float width = GENERATE(1280.0F, 320.0F, 80.0F);
    EditorWorkspaceViewCommandData command;
    fixture.imgui.BeginFrame();
    view.Draw(model, command, GuiContentRegion{0, 0, width, 480});
    fixture.imgui.EndFrame();
    CHECK(command.command == EditorWorkspaceViewCommand::None);
    for (const auto &panel : {left, right, bottom}) {
        CHECK(panel->lastSize.x >= 0.0F);
        CHECK(panel->lastSize.x <= width);
        CHECK(panel->lastSize.y >= 0.0F);
        CHECK(panel->lastSize.y <= 480.0F);
    }
}

TEST_CASE("Workspace document strip retains long source identities and exposes narrow overflow",
          "[unit][editor][gui][workspace][documents]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Horo::Editor::Tests::EditorGuiContextFixture fixture;
    WorkspacePanelRegistry panels;
    auto input = fixture.input.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
    EditorWorkspaceView view{fixture.context, panels, 0, fixture.input, input};
    EditorWorkspaceViewModel model;
    const Horo::Tests::OwnedTestDirectory directory{"workspace-source-presentation"};
    {
        std::ofstream source(directory.Path() / "preview.cpp");
        source << std::string(65'535, 'a') << "\xC3\xBC";
    }
    SourceDocumentService documents{directory.Path()};
    const auto preview = model.workspacePanelHost.OpenDocument({DocumentKind::Source, SourceDocumentId::Parse("preview.cpp").Value()});
    REQUIRE(preview.HasValue());
    const auto source = documents.Open(preview.Value().identity, directory.Path() / "preview.cpp");
    REQUIRE(source.HasValue());
    model.sourceDocuments.push_back(source.Value());
    for (int index = 0; index < 8; ++index) {
        const auto path = "source/" + std::to_string(index) + "/LongLocalizedSourceDocumentName.cpp";
        REQUIRE(model.workspacePanelHost.OpenDocument({DocumentKind::Source, SourceDocumentId::Parse(path).Value()}).HasValue());
    }
    REQUIRE(model.workspacePanelHost.FocusDocument(preview.Value().identity.instance).HasValue());
    REQUIRE(model.workspacePanelHost.SetDocumentDirty(preview.Value().identity.instance, true).HasValue());
    const float width = GENERATE(1280.0F, 320.0F);
    for (int frame = 0; frame < 2; ++frame) {
        EditorWorkspaceViewCommandData command;
        fixture.imgui.BeginFrame();
        view.Draw(model, command, GuiContentRegion{0, 0, width, 600});
        fixture.imgui.EndFrame();
        CHECK(command.command == EditorWorkspaceViewCommand::None);
    }
    bool overflow = false;
    for (const auto *window : ImGui::GetCurrentContext()->Windows)
        if (std::string_view{window->Name}.find("DocumentTabStrip") != std::string_view::npos)
            overflow = window->ScrollMax.x > 0;
    CHECK(overflow);
    CHECK(model.workspacePanelHost.DocumentTabs().size() == 9);
    CHECK(model.workspacePanelHost.ActiveDocument() == preview.Value().identity.instance);
    CHECK(source.Value().Text().size() == 65'537);
    CheckSourceSaveAndUnavailable(fixture, view, model, source.Value(), width);
}

TEST_CASE("Loading recovery actions keep padding and failures use error colors", "[unit][editor][gui][project-loading-layout]") {
    using namespace Horo::Editor;
    const auto locale = GENERATE("en-US", "tr-TR");
    const int scale = GENERATE(100, 150, 200);
    const float viewportWidth = GENERATE(1280.0F, 360.0F);
    Tests::EditorGuiContextFixture fixture{{viewportWidth, 1000.0F}};

    struct RestoreScale {
        ~RestoreScale() {
            Theme::SetUiScalePercent(100);
        }
    } restore;

    Theme::SetUiScalePercent(scale);
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
    REQUIRE(fixture.localization.Prepare(LocaleTag{locale}));
    REQUIRE(fixture.localization.ActivatePrepared());
    ProjectLoadingViewState state{.projectName = "SuperProject28",
                                  .progress = 100.0F,
                                  .statusText = "Derived project state could not be rebuilt safely.",
                                  .hasFailed = true,
                                  .canRetry = true};
    const auto draw = [&] {
        fixture.imgui.BeginFrame();
        const auto command = DrawProjectLoadingView(state, fixture.context, {0, 0, viewportWidth, 1000});
        fixture.imgui.EndFrame();
        return command;
    };
    REQUIRE(draw() == ProjectLoadingViewCommand::None);
    REQUIRE(draw() == ProjectLoadingViewCommand::None);
    ImGuiWindow *modal{};
    for (auto *window : GImGui->Windows)
        if (std::string_view{window->Name}.find("/LoadingModal_") != std::string_view::npos)
            modal = window;
    REQUIRE(modal != nullptr);
    const auto &tokens = Theme::GetActiveTokens();
    const auto &metrics = DesignSystem::MetricsFor(tokens, DesignSystem::ComponentSize::Medium);
    const float inset = DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Large);
    float buttonTop = FLT_MAX;
    float progressBottom = -FLT_MAX;
    const auto whitePixel = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
    float buttonRight = -FLT_MAX;
    float buttonBottom = -FLT_MAX;
    float textRight = -FLT_MAX;
    int errorVertices{};
    const float footerTop = modal->Pos.y + modal->Size.y - inset - metrics.minimumHeight;
    for (const auto &vertex : modal->DrawList->VtxBuffer) {
        if (vertex.col == ImGui::GetColorU32(Theme::Err())) {
            ++errorVertices;
            if (vertex.uv.x == whitePixel.x && vertex.uv.y == whitePixel.y)
                progressBottom = std::max(progressBottom, vertex.pos.y);
        }
        if (vertex.col == ImGui::GetColorU32(Theme::Bg3())) {
            buttonTop = std::min(buttonTop, vertex.pos.y);
            buttonRight = std::max(buttonRight, vertex.pos.x);
            buttonBottom = std::max(buttonBottom, vertex.pos.y);
        }
        if (vertex.col == ImGui::GetColorU32(Theme::Text()) && vertex.pos.y >= footerTop)
            textRight = std::max(textRight, vertex.pos.x);
    }
    INFO(locale << ", scale=" << scale << ", width=" << viewportWidth);
    CHECK(errorVertices > 12);  // Status text, percentage, and filled progress bar all carry error color.
    CHECK(buttonTop - progressBottom == Catch::Approx(DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Medium)).margin(2.0F));
    CHECK(buttonRight == Catch::Approx(modal->Pos.x + modal->Size.x - inset).margin(1.5F));
    CHECK(buttonBottom == Catch::Approx(modal->Pos.y + modal->Size.y - inset).margin(1.5F));
    CHECK(textRight > modal->Pos.x);
    CHECK(buttonRight - textRight >= metrics.paddingX - 2.0F);
    CHECK(modal->Pos.y + modal->Size.y <= 1000.0F);
    ImGui::ActivateItemByID(modal->GetID(fixture.localization.Get("editor", "project_loading.action.retry").c_str()));
    CHECK(draw() == ProjectLoadingViewCommand::Retry);
    ImGui::ActivateItemByID(modal->GetID(fixture.localization.Get("editor", "project_loading.action.back").c_str()));
    CHECK(draw() == ProjectLoadingViewCommand::Back);
}

TEST_CASE("Workspace transport buttons dispatch only canonical lifecycle actions from stable slots", "[editor][gui][workspace][toolbar]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Horo::Editor::Tests::EditorGuiContextFixture fixture;
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json"));
    REQUIRE(fixture.localization.Prepare(LocaleTag{"en-US"}));
    REQUIRE(fixture.localization.ActivatePrepared());
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    WorkspacePanelRegistry panels;
    auto input = fixture.input.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
    EditorWorkspaceView view{fixture.context, panels, 0, fixture.input, input};
    EditorWorkspaceViewModel model;
    model.projectRoot = "/projects/example";
    model.projectName = "Very long localized project name — Uzun Proje Adı";
    const GuiContentRegion content{0, 0, 1280, 800};
    const auto frame = [&](const char *control) {
        fixture.imgui.BeginFrame();
        if (control) {
            auto *bar = ImGui::FindWindowByName("##MenuBar");
            REQUIRE(bar);
            const auto menuScope = ImHashStr("##menubar", 0, bar->ID);
            const auto scope = ImHashStr(control, 0, menuScope);
            const auto id = ImHashStr("##icon-button", 0, scope);
            ImGui::FocusWindow(bar);
            ImGui::SetFocusID(id, bar);
            auto &gui = *ImGui::GetCurrentContext();
            gui.NavLayer = ImGuiNavLayer_Menu;
            gui.NavActivateId = gui.NavActivateDownId = gui.NavActivatePressedId = id;
        }
        EditorWorkspaceViewCommandData command;
        view.Draw(model, command, content);
        fixture.imgui.EndFrame();
        return command.command;
    };
    frame(nullptr);
    frame(nullptr);
    {
        const auto *bar = ImGui::FindWindowByName("##MenuBar");
        REQUIRE(bar);
        auto disabledTone = Theme::Text();
        disabledTone.w *= 0.45F;
        const ImU32 disabledColor = Theme::U32(disabledTone);
        float top = 1000.0F;
        float bottom = -1000.0F;
        for (const auto &vertex : bar->DrawList->VtxBuffer) {
            if (vertex.col == disabledColor) {
                top = std::min(top, vertex.pos.y);
                bottom = std::max(bottom, vertex.pos.y);
            }
        }
        REQUIRE(bottom > top);
        CHECK((top + bottom) * 0.5F == Catch::Approx(bar->Pos.y + bar->Size.y * 0.5F).margin(0.6F));
    }

    struct Scenario {
        EditorPlayState state;
        const char *control;
        EditorWorkspaceViewCommand expected;
    };

    const std::array scenarios{Scenario{EditorPlayState::Idle, "workspace_play_toggle", EditorWorkspaceViewCommand::StartPlay},
                               Scenario{EditorPlayState::Playing, "workspace_play_toggle", EditorWorkspaceViewCommand::StopPlay},
                               Scenario{EditorPlayState::Paused, "workspace_play_toggle", EditorWorkspaceViewCommand::StopPlay},
                               Scenario{EditorPlayState::Playing, "workspace_play_pause", EditorWorkspaceViewCommand::PausePlay},
                               Scenario{EditorPlayState::Paused, "workspace_play_pause", EditorWorkspaceViewCommand::ResumePlay},
                               Scenario{EditorPlayState::Paused, "workspace_play_step", EditorWorkspaceViewCommand::StepPlay},
                               Scenario{EditorPlayState::Idle, "workspace_play_step", EditorWorkspaceViewCommand::None},
                               Scenario{EditorPlayState::Playing, "workspace_play_step", EditorWorkspaceViewCommand::None},
                               Scenario{EditorPlayState::Starting, "workspace_play_toggle", EditorWorkspaceViewCommand::None},
                               Scenario{EditorPlayState::Stopping, "workspace_play_pause", EditorWorkspaceViewCommand::None},
                               Scenario{EditorPlayState::Failed, "workspace_play_toggle", EditorWorkspaceViewCommand::StartPlay}};
    for (const auto &scenario : scenarios) {
        model.playState = scenario.state;
        INFO(static_cast<int>(scenario.state) << " " << scenario.control);
        CHECK(frame(scenario.control) == scenario.expected);
    }
    for (const float width : {360.0F, 640.0F, 1280.0F}) {
        fixture.imgui.BeginFrame();
        EditorWorkspaceViewCommandData command;
        view.Draw(model, command, GuiContentRegion{0, 0, width, 800});
        fixture.imgui.EndFrame();
        const auto *bar = ImGui::FindWindowByName("##MenuBar");
        REQUIRE(bar);
        CHECK(bar->ScrollMax.x == 0.0F);
    }
    model.projectRoot.clear();
    model.playState = EditorPlayState::Idle;
    CHECK(frame("workspace_play_toggle") == EditorWorkspaceViewCommand::None);
}

TEST_CASE("Leave confirmations use padded shared modal chrome and localized responsive actions", "[editor][gui][leave-confirmation]") {
    using namespace Horo::Editor;
    const float width = GENERATE(280.0F, 800.0F);
    const auto locale = GENERATE("en-US", "tr-TR");
    Tests::EditorGuiContextFixture fixture{{width, 720}};
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
    REQUIRE(fixture.localization.Prepare(LocaleTag{locale}));
    REQUIRE(fixture.localization.ActivatePrepared());
    const LeaveRequirement requirement{LeaveRequirementKind::DirtyDocument,
                                       1,
                                       1,
                                       {LeaveAction::Save, LeaveAction::Discard, LeaveAction::Stay}};
    for (int frame = 0; frame < 3; ++frame) {
        fixture.imgui.BeginFrame();
        CHECK_FALSE(DrawLeaveConfirmation(requirement, fixture.context).has_value());
        fixture.imgui.EndFrame();
        const auto *modal = ImGui::FindWindowByName("LeaveConfirmation");
        REQUIRE(modal);
        CHECK((modal->Flags & ImGuiWindowFlags_Modal) != 0);
        CHECK((modal->Flags & ImGuiWindowFlags_NoTitleBar) != 0);
        CHECK(modal->ScrollMax.x == 0);
        bool foundBody = false;
        bool foundFooter = false;
        for (const auto *window : ImGui::GetCurrentContext()->Windows) {
            if (window->ParentWindow != modal)
                continue;
            const std::string_view name{window->Name};
            if (name.find("##LeaveBody") != std::string_view::npos) {
                foundBody = true;
                CHECK(window->WindowPadding.x == Catch::Approx(Ui::ScaledLayoutValue(20)));
                CHECK(window->WindowPadding.y == Catch::Approx(Ui::ScaledLayoutValue(16)));
                CHECK(window->ScrollMax.x == 0);
            }
            if (name.find("##ModalFooter") != std::string_view::npos) {
                foundFooter = true;
                CHECK(window->WindowPadding.x == Catch::Approx(Ui::ScaledLayoutValue(20)));
                CHECK(window->DC.CursorMaxPos.x <= window->WorkRect.Max.x + 1);
                CHECK(window->ScrollMax.x == 0);
            }
        }
        CHECK(foundBody);
        CHECK(foundFooter);
    }
}

TEST_CASE("Leave confirmation close actions map only to an allowed Stay action", "[editor][gui][leave-confirmation]") {
    using namespace Horo::Editor;
    const bool allowStay = GENERATE(true, false);
    const bool useEscape = GENERATE(true, false);
    Tests::EditorGuiContextFixture fixture;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json"));
    REQUIRE(fixture.localization.Prepare(LocaleTag{"en-US"}));
    REQUIRE(fixture.localization.ActivatePrepared());
    LeaveRequirement requirement{LeaveRequirementKind::DirtyDocument, 1, 1, {LeaveAction::Save, LeaveAction::Discard}};
    if (allowStay)
        requirement.allowedActions.push_back(LeaveAction::Stay);
    std::optional<LeaveAction> selected;
    for (int frame = 0; frame < 3; ++frame) {
        if (frame == 2 && useEscape)
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        fixture.imgui.BeginFrame();
        if (frame == 2 && !useEscape) {
            auto &gui = *ImGui::GetCurrentContext();
            const auto *modal = ImGui::FindWindowByName("LeaveConfirmation");
            REQUIRE(modal);
            for (auto *window : gui.Windows) {
                if (window->ParentWindow != modal || std::string_view{window->Name}.find("##ModalHeader") == std::string_view::npos)
                    continue;
                const auto id = ImHashStr("##close", 0, ImHashStr("##ModalClose", 0, window->ID));
                ImGui::FocusWindow(window);
                ImGui::SetFocusID(id, window);
                gui.NavLayer = ImGuiNavLayer_Main;
                gui.NavActivateId = gui.NavActivateDownId = gui.NavActivatePressedId = id;
            }
        }
        selected = DrawLeaveConfirmation(requirement, fixture.context);
        fixture.imgui.EndFrame();
    }
    if (allowStay) {
        REQUIRE(selected.has_value());
        CHECK(*selected == LeaveAction::Stay);
        CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    } else
        CHECK_FALSE(selected.has_value());
}
