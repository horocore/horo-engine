#include "EditorActivityPackageSupport.h"
#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "SecurityTestSupport.h"
#include "editor/modals/settings/SettingsModalInternal.h"
#include "editor/project_model/RendererAvailability.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <imgui_internal.h>
#include <iterator>

TEST_CASE("GUI composition admits package themes only after activation and withdraws their stable identity on shutdown",
          "[editor][extensions][theme-integration]") {
    using namespace Horo;
    using namespace Horo::Editor;
    Horo::Tests::EditorActivityPackage package;
    std::ifstream manifestInput{package.root / "extension.json"};
    std::string manifest{std::istreambuf_iterator<char>{manifestInput}, {}};
    manifest.insert(manifest.rfind("]}"),
                    R"(,{"type":"editor.theme","id":"fixture.theme","module":"fixture.module","resource":"themes/fixture.json"})");
    std::ofstream{package.root / "extension.json"} << manifest;
    std::filesystem::create_directory(package.root / "themes");
    const bool validJson = GENERATE(false, true);
    std::ofstream{package.root / "themes/fixture.json"}
        << (validJson ? R"({"name":"Fixture theme","tokens":{"sourceSyntax":{"keyword":"#AA77FF"}}})" : "malformed");
    Horo::Tests::OwnedTestDirectory installed{"editor theme package inventory"};
    Extensions::ExtensionInventory inventory{installed.Path()};
    REQUIRE(inventory.InstallFromDirectory(package.root).HasValue());
    REQUIRE(inventory.SetEnabled("fixture.package", true).HasValue());
    REQUIRE(inventory.SetTrusted("fixture.package", true).HasValue());
    Horo::Editor::Tests::EditorGuiContextFixture gui;
    Theme::RefreshThemeList();
    gui.settings.settings.themeId = "fixture.theme";
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
    ProjectCreationService creation{jobs, gui.engineEvents};
    auto configuration = CreateEditorConfigurationService(gui.settings.settings);
    EditorSettingsService settings{gui.settings.settings, configuration, gui.editorEvents, gui.localization};
    EditorModalHost modals{gui.editorEvents, gui.input};
    RendererAvailabilitySnapshot renderers{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}},
                                           "opengl"};
    const bool trustedArtifact = GENERATE(false, true);
    GuiScreenHost host{gui.context, GuiScreenHostComposition{.modalHost = modals,
                                                             .settingsService = settings,
                                                             .localization = gui.localization,
                                                             .engineEvents = gui.engineEvents,
                                                             .creationService = creation,
                                                             .jobs = jobs,
                                                             .inputRouter = gui.input,
                                                             .rendererAvailability = renderers,
                                                             .screenRegistry = {},
                                                             .workspacePanelRegistry = {},
                                                             .extensionInventory = &inventory,
                                                             .extensionArtifactGate =
                                                                 trustedArtifact ? Horo::Tests::CreateAcceptingArtifactGate() : nullptr,
                                                             .extensionUiPolicy = Horo::Tests::EditorCompositionPolicy()}};
    const auto &themes = Theme::GetThemeList();
    const auto found = std::ranges::find(themes, std::string{"fixture.theme"}, &Theme::ThemeEntry::id);
    CHECK((found != themes.end()) == (trustedArtifact && validJson));
    if (found != themes.end())
        CHECK(Theme::GetThemeList()[static_cast<std::size_t>(Theme::GetActiveThemeIndex())].id == "fixture.theme");
    host.Shutdown();
    CHECK_FALSE(Theme::SelectThemeById("fixture.theme"));
    CHECK(settings.Snapshot().settings.themeId == "fixture.theme");
    jobs.Shutdown(ShutdownPolicy::Cancel);
}

TEST_CASE("Settings destinations with equal translated labels retain distinct widget identities",
          "[editor][settings][navigation][identity]") {
    using namespace Horo::Editor;
    using namespace Horo::Editor::SettingsModalInternal;
    Horo::Editor::Tests::EditorGuiContextFixture gui{{700, 500}};
    SettingsState state;
    ImVec2 appearance{}, codeAppearance{};
    int matchingItems{};
    const auto frame = [&] {
        gui.imgui.BeginFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({650, 450});
        ImGui::Begin("Settings identity regression", nullptr, ImGuiWindowFlags_NoSavedSettings);
        appearance = ImGui::GetCursorScreenPos();
        DrawNavItem(state, {"Appearance", "A", SettingsTab::Appearance}, gui.context);
        codeAppearance = ImGui::GetCursorScreenPos();
        DrawNavItem(state, {"Appearance", "A", SettingsTab::CodeEditorAppearance}, gui.context);
        matchingItems = ImGui::GetCurrentContext()->HoveredIdPreviousFrameItemCount;
        ImGui::End();
        gui.imgui.EndFrame();
    };
    frame();
    frame();
    for (const auto &position : {appearance, codeAppearance}) {
        ImGui::GetIO().AddMousePosEvent(position.x + 20, position.y + 16);
        frame();
        frame();
        REQUIRE(matchingItems == 1);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        CHECK(state.activeTab ==
              static_cast<int>(position.y == appearance.y ? SettingsTab::Appearance : SettingsTab::CodeEditorAppearance));
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
    }
}

TEST_CASE("Code preferences share typed descriptors with modal drafts", "[editor][settings][code]") {
    using namespace Horo::Editor;
    auto settings = DefaultEditorSettings();
    settings.codeFontFamily = "Menlo";
    settings.uiFontFamily = "Custom UI";
    for (const auto &entry : CodeEditorBooleanSettings())
        settings.codeEditor.*entry.member = !(settings.codeEditor.*entry.member);
    settings.codeEditor.autoSaveDelaySeconds = 3;
    SettingsState draft;
    ApplySettingsToDraft(draft, settings);
    CHECK(CollectDraftSettings(draft) == settings);
}
