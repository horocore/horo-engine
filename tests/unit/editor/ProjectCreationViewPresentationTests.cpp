#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/GuiScreen.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/ProjectCreationController.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Runtime/Input.h"
#include "editor/project_model/RendererAvailability.h"
#include "editor/screens/project_creation/ProjectCreationView.h"
#include "editor/status_bar/EditorStatusBar.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <imgui_internal.h>

namespace {
    Horo::Editor::ProjectCreationViewCommand DrawProjectCreationFrame(Horo::Editor::Tests::EditorGuiContextFixture &fixture,
                                                                      Horo::Editor::ProjectCreationController &controller,
                                                                      Horo::Editor::ProjectCreationViewState &state,
                                                                      const Horo::Editor::RendererAvailabilitySnapshot &renderers,
                                                                      const Horo::Editor::GuiContentRegion &content) {
        fixture.imgui.BeginFrame();
        const auto command = Horo::Editor::DrawProjectCreationView(controller, state, fixture.context, fixture.input, renderers, content);
        fixture.imgui.EndFrame();
        return command;
    }
}  // namespace

TEST_CASE("Project creation presentation renders every wizard step and template-specific review", "[unit][editor][gui][project-creation]") {
    using namespace Horo;
    using namespace Horo::Editor;

    Tests::EditorGuiContextFixture fixture;
    const RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}},
                                                  RendererBackendAvailability{"metal", "Metal", RendererAvailabilityState::Available, {}},
                                                  RendererBackendAvailability{"vulkan", "Vulkan", RendererAvailabilityState::NotInstalled,
                                                                              "Not installed"}},
                                                 "opengl"};
    ProjectCreationController controller{renderers};
    controller.SetProjectName("PresentationProject");
    controller.SetProjectPath("/tmp/horo-presentation-project");
    const GuiContentRegion content{0.0F, 0.0F, 1280.0F, 800.0F};

    ProjectCreationViewState state;
    state.highestUnlockedStep = 4;
    for (int step = 1; step <= 4; ++step) {
        state.step = step;
        const ProjectCreationViewCommand command = DrawProjectCreationFrame(fixture, controller, state, renderers, content);
        REQUIRE(command == ProjectCreationViewCommand::None);
        REQUIRE(state.step == step);
    }

    static constexpr std::array templates{"package-based", "first-person", "tech-demo", "custom"};
    for (const char *templateId : templates) {
        controller.SetTemplateId(templateId);
        state.step = 4;
        const ProjectCreationViewCommand command = DrawProjectCreationFrame(fixture, controller, state, renderers, content);
        REQUIRE(command == ProjectCreationViewCommand::None);
        REQUIRE(controller.Draft().templateId == templateId);
    }
}

TEST_CASE("Project creation presentation normalizes an out-of-range step", "[unit][editor][gui][project-creation]") {
    using namespace Horo;
    using namespace Horo::Editor;

    Tests::EditorGuiContextFixture fixture{{640.0F, 480.0F}};
    const RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}},
                                                 "opengl"};
    ProjectCreationController controller{renderers};
    ProjectCreationViewState state;
    state.step = 99;

    static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, GuiContentRegion{0.0F, 0.0F, 640.0F, 480.0F}));

    REQUIRE(state.step == 1);
}

TEST_CASE("Project creation presentation keeps review locked while required identity fields are incomplete",
          "[unit][editor][gui][project-creation]") {
    using namespace Horo;
    using namespace Horo::Editor;

    Tests::EditorGuiContextFixture fixture;
    const RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}},
                                                 "opengl"};
    ProjectCreationController controller{renderers};
    ProjectCreationViewState state;
    state.step = 4;
    state.highestUnlockedStep = 4;

    static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, GuiContentRegion{0.0F, 0.0F, 1280.0F, 800.0F}));

    REQUIRE(state.step == 2);
}

TEST_CASE("Template cards retain bottom padding for wrapped localized descriptions", "[unit][editor][gui][project-creation-padding]") {
    using namespace Horo::Editor;
    const auto locale = GENERATE("en-US", "tr-TR");
    const float width = GENERATE(1100.0F, 1500.0F);
    Tests::EditorGuiContextFixture fixture{{width, 900}};
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
    REQUIRE(fixture.localization.Prepare(LocaleTag{locale}));
    REQUIRE(fixture.localization.ActivatePrepared());
    const RendererAvailabilitySnapshot renderers{{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"};
    ProjectCreationController controller{renderers};
    ProjectCreationViewState state;
    for (int frame = 0; frame < 2; ++frame)
        static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, {0, 0, width, 900}));
    int cards{};
    for (auto *window : GImGui->Windows) {
        if (!window->Active || std::string_view{window->Name}.find("/TemplateCard_") == std::string_view::npos)
            continue;
        ++cards;
        INFO(locale << ", width=" << width << ", card=" << window->Name);
        CHECK(window->ContentSize.y + window->WindowPadding.y * 2 <= window->Size.y + 1.0F);
    }
    CHECK(cards == 6);
}

TEST_CASE("Project wizard uses the full route and restores shell status on exit", "[unit][editor][gui][project-creation-chrome]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Tests::EditorGuiContextFixture fixture;
    JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8}};
    ProjectCreationService creation{jobs, fixture.engineEvents};
    auto configuration = CreateEditorConfigurationService(DefaultEditorSettings());
    EditorSettingsService settings{DefaultEditorSettings(), configuration, fixture.editorEvents, fixture.localization};
    EditorModalHost modals{fixture.editorEvents, fixture.input};
    const RendererAvailabilitySnapshot renderers{{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"};
    GuiContentRegion received;

    class RecordingScreen final : public GuiScreen {
    public:
        RecordingScreen(GuiContentRegion &region, GuiRouteKind kind) : GuiScreen("test.wizardChrome"), region{region}, kind{kind} {}

        ScreenId Id() const override {
            return static_cast<ScreenId>(kind);
        }

        Result<void> OnEnter(const GuiRoute &) override {
            return Result<void>::Success();
        }

        void OnUpdate(float) override {}

        void Draw(const GuiContentRegion &content) override {
            region = content;
        }

        LeaveDecision CanLeave(const LeaveTarget &) const override {
            return {LeaveDisposition::Allow, std::nullopt};
        }

        Result<LeaveDecision> ResolveLeave(const LeaveTarget &, const LeaveResolution &) override {
            return Result<LeaveDecision>::Success({LeaveDisposition::Allow, std::nullopt});
        }

        void OnLeave() override {}

        GuiContentRegion &region;
        GuiRouteKind kind;
    };

    ScreenRegistry screens;
    for (auto kind : {GuiRouteKind::Welcome, GuiRouteKind::ProjectCreation})
        screens.Register(kind, [&](const EditorServiceRegistry &, const GuiRoute &route) {
            return std::make_unique<RecordingScreen>(received, route.kind);
        });
    GuiScreenHost host{fixture.context, GuiScreenHostComposition{.modalHost = modals,
                                                                 .settingsService = settings,
                                                                 .localization = fixture.localization,
                                                                 .engineEvents = fixture.engineEvents,
                                                                 .creationService = creation,
                                                                 .jobs = jobs,
                                                                 .inputRouter = fixture.input,
                                                                 .rendererAvailability = renderers,
                                                                 .screenRegistry = std::move(screens),
                                                                 .workspacePanelRegistry = WorkspacePanelRegistry{}}};
    REQUIRE(host.Start({GuiRouteKind::ProjectCreation, ProjectCreationRouteParameters{}}).HasValue());
    for (bool wizard : {true, false, true}) {
        if (wizard != (host.ActiveRoute().kind == GuiRouteKind::ProjectCreation)) {
            const GuiRoute route = wizard ? GuiRoute{GuiRouteKind::ProjectCreation, ProjectCreationRouteParameters{}}
                                          : GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}};
            REQUIRE(host.Navigate(route).HasValue());
        }
        fixture.imgui.BeginFrame();
        host.Draw();
        const auto *status = ImGui::FindWindowByName("##EditorStatusBar");
        CHECK((status && status->Active) == !wizard);
        CHECK(received.height == ImGui::GetMainViewport()->WorkSize.y - (wizard ? 0.0F : EditorStatusBar::Height));
        fixture.imgui.EndFrame();
    }
}

TEST_CASE("Identity fields defer errors until editing finishes and folder action matches the input",
          "[unit][editor][gui][project-creation-feedback]") {
    using namespace Horo::Editor;
    const auto locale = GENERATE("en-US", "tr-TR");
    Tests::EditorGuiContextFixture fixture;
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
    REQUIRE(fixture.localization.Prepare(LocaleTag{locale}));
    REQUIRE(fixture.localization.ActivatePrepared());
    const RendererAvailabilitySnapshot renderers{{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"};
    ProjectCreationController controller{renderers};
    controller.SetProjectName("");
    controller.SetProjectPath("");
    ProjectCreationViewState state;
    state.step = state.highestUnlockedStep = 2;
    const auto frame = [&] {
        static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, {0, 0, 1280, 800}));
    };
    const auto errorVertices = [&] {
        int count{};
        for (const auto *window : GImGui->Windows)
            if (window->Active)
                for (const auto &vertex : window->DrawList->VtxBuffer)
                    count += vertex.col == ImGui::GetColorU32(Theme::Err());
        return count;
    };
    frame();
    frame();
    CHECK_FALSE(controller.Validate().IsValid());
    CHECK(errorVertices() == 0);
    ImGuiWindow *main{};
    for (auto *window : GImGui->Windows)
        if (std::string_view{window->Name}.find("/WizMain_") != std::string_view::npos)
            main = window;
    REQUIRE(main != nullptr);
    const auto nameScope = main->GetID(fixture.localization.Get("editor", "project_creation.identity.name").c_str());
    const auto nameId = ImHashStr("###project_creation_name", 0, nameScope);
    const auto pathScope = main->GetID(fixture.localization.Get("editor", "project_creation.identity.location").c_str());
    const auto pathId = ImHashStr("###project_creation_location", 0, pathScope);
    const auto folderId = ImHashStr("##btn", 0, ImHashStr("##browse_location", 0, pathScope));
    ImGui::SetFocusID(pathId, main);
    frame();
    const auto pathRect = main->NavRectRel[ImGuiNavLayer_Main];
    ImGui::SetFocusID(folderId, main);
    frame();
    const auto folderRect = main->NavRectRel[ImGuiNavLayer_Main];
    CHECK(pathRect.Min.y == folderRect.Min.y);
    CHECK(pathRect.Max.y == folderRect.Max.y);
    CHECK(pathRect.GetHeight() > 0);
    CHECK(pathRect.GetWidth() > folderRect.GetWidth());
    ImGui::SetFocusID(nameId, main);
    ImGui::ActivateItemByID(nameId);
    frame();
    ImGui::GetIO().AddInputCharactersUTF8("bad/name");
    frame();
    REQUIRE(state.projectName == "bad/name");
    CHECK(errorVertices() == 0);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, true);
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, false);
    frame();
    frame();
    CHECK(errorVertices() > 0);
    controller.SetProjectName("");
    state = {};
    state.step = state.highestUnlockedStep = 2;
    ImGui::ClearActiveID();
    frame();
    frame();
    CHECK(errorVertices() == 0);
}

TEST_CASE("Identity resolves the project folder in either input order and follows renames",
          "[unit][editor][gui][project-creation-location]") {
    using namespace Horo::Editor;
    const bool folderFirst = GENERATE(true, false);
    const auto locale = GENERATE("en-US", "tr-TR");
    Tests::EditorGuiContextFixture fixture;
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json"));
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/tr-TR.json"));
    REQUIRE(fixture.localization.Prepare(LocaleTag{locale}));
    REQUIRE(fixture.localization.ActivatePrepared());
    for (const auto key : {"project_creation.identity.location", "project_creation.identity.location.select",
                           "project_creation.identity.name.invalid_folder"}) {
        REQUIRE(fixture.localization.Get("editor", key) != key);
        REQUIRE_FALSE(fixture.localization.Get("editor", key).empty());
    }
    const RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}},
                                                 "opengl"};
    ProjectCreationController controller{renderers};
    ProjectCreationViewState state;
    state.step = state.highestUnlockedStep = 2;
    const GuiContentRegion content{0.0F, 0.0F, 1280.0F, 800.0F};
    static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, content));
    const auto parent = std::filesystem::temp_directory_path();
    if (folderFirst)
        state.projectParentDirectory = parent.string();
    else
        state.projectName = "Horo Wizard Journey Project";
    static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, content));
    REQUIRE(controller.Draft().projectPath.empty());
    state.projectParentDirectory = parent.string();
    state.projectName = "Horo Wizard Journey Project";
    static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, content));
    REQUIRE(std::filesystem::path{controller.Draft().projectPath} == parent / state.projectName);
    const auto request = controller.BuildCreationRequest();
    REQUIRE(request.has_value());
    REQUIRE(request->projectRoot == parent / state.projectName);
    state.projectName = "Horo Renamed Journey Project";
    static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, content));
    REQUIRE(std::filesystem::path{controller.Draft().projectPath} == parent / state.projectName);
    REQUIRE(state.projectParentDirectory == parent.string());
    state.projectName.clear();
    static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, content));
    REQUIRE(controller.Draft().projectPath.empty());
}

TEST_CASE("Project Folder input displays the final path and preserves an explicitly typed custom folder",
          "[unit][editor][gui][project-creation-location]") {
    using namespace Horo::Editor;
    const float width = GENERATE(850.0F, 1280.0F);
    Tests::EditorGuiContextFixture fixture{{width, 850}};
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json"));
    REQUIRE(fixture.localization.Prepare(LocaleTag{"en-US"}));
    REQUIRE(fixture.localization.ActivatePrepared());
    REQUIRE(fixture.localization.Get("editor", "project_creation.identity.location") == "PROJECT FOLDER");
    const RendererAvailabilitySnapshot renderers{{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"};
    ProjectCreationController controller{renderers};
    controller.SetProjectName("Initial Project");
    const auto parent = std::filesystem::temp_directory_path();
    controller.SetProjectParentDirectory(parent);
    ProjectCreationViewState state;
    state.step = state.highestUnlockedStep = 2;
    const auto frame = [&] {
        static_cast<void>(DrawProjectCreationFrame(fixture, controller, state, renderers, {0, 0, width, 850}));
    };
    frame();
    frame();
    ImGuiWindow *main{};
    for (auto *window : GImGui->Windows)
        if (std::string_view{window->Name}.find("/WizMain_") != std::string_view::npos)
            main = window;
    REQUIRE(main != nullptr);
    const auto scope = main->GetID(fixture.localization.Get("editor", "project_creation.identity.location").c_str());
    const auto pathId = ImHashStr("###project_creation_location", 0, scope);
    const auto focusPath = [&] {
        ImGui::SetFocusID(pathId, main);
        ImGui::ActivateItemByID(pathId);
        frame();
        REQUIRE(ImGui::GetInputTextState(pathId) != nullptr);
    };
    const auto replacePath = [&](const std::string &path) {
        focusPath();
        auto &io = ImGui::GetIO();
        const auto modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        io.AddKeyEvent(modifier, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        frame();
        REQUIRE(ImGui::GetInputTextState(pathId)->HasSelection());
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(modifier, false);
        frame();
        if (path.empty()) {
            io.AddKeyEvent(ImGuiKey_Backspace, true);
            frame();
            io.AddKeyEvent(ImGuiKey_Backspace, false);
        } else {
            io.AddInputCharactersUTF8(path.c_str());
        }
        frame();
        REQUIRE(std::string_view{ImGui::GetInputTextState(pathId)->TextA.Data} == path);
        ImGui::ClearActiveID();
        frame();
    };
    focusPath();
    REQUIRE(std::string_view{ImGui::GetInputTextState(pathId)->TextA.Data} == (parent / "Initial Project").string());
    ImGui::ClearActiveID();
    state.projectName = "Renamed Project";
    frame();
    focusPath();
    REQUIRE(std::string_view{ImGui::GetInputTextState(pathId)->TextA.Data} == (parent / "Renamed Project").string());
    ImGui::ClearActiveID();
    const auto custom = parent / "Explicit Folder";
    replacePath(custom.string());
    state.projectName = "Another Name";
    frame();
    REQUIRE(std::filesystem::path{controller.Draft().projectPath} == custom);
    const auto newParent = parent / "New Parent";
    replacePath((newParent / state.projectName).string() + "/");
    state.projectName = "Final Name";
    frame();
    REQUIRE(std::filesystem::path{controller.Draft().projectPath} == newParent / "Final Name");
    replacePath("");
    state.projectName = "After Clearing";
    frame();
    REQUIRE(controller.Draft().projectPath.empty());
    REQUIRE(state.projectParentDirectory.empty());
}
