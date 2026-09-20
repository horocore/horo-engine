#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "editor/modals/settings/SettingsModalInternal.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <stdexcept>

namespace {
    struct SettingsPresentationFixture {
        SettingsPresentationFixture()
            : settings{Horo::Editor::DefaultEditorSettings(), configuration, gui.editorEvents, gui.localization},
              modal{gui.context, settings, 0} {
            if (const auto refreshed = extensions.Refresh(); refreshed.HasError())
                throw std::runtime_error(refreshed.ErrorValue().message);
            Horo::Editor::LoadSettingsForModal(modal.Draft(), settings);
            modal.Draft().extensionInventory = &extensions;
        }

        Horo::Editor::Tests::EditorGuiContextFixture gui;
        Horo::ConfigurationService configuration = Horo::Editor::CreateEditorConfigurationService(Horo::Editor::DefaultEditorSettings());
        Horo::Editor::EditorSettingsService settings;
        Horo::Extensions::ExtensionInventory extensions{std::filesystem::temp_directory_path() / "horo-settings-presentation-extensions"};
        Horo::Editor::SettingsModal modal;
    };

    void DrawPluginWhiteBoxFrame(SettingsPresentationFixture &fixture, const int selectedPlugin, const int detailTab) {
        using namespace Horo::Editor::SettingsModalInternal;

        auto &state = fixture.modal.Draft();
        state.selectedPlugin = selectedPlugin;
        state.pluginDetailTab[static_cast<std::size_t>(selectedPlugin)] = detailTab;
        fixture.gui.imgui.BeginFrame();
        ImGui::SetNextWindowSize({1200.0F, 760.0F}, ImGuiCond_Always);
        ImGui::Begin("SettingsPluginPresentation", nullptr, ImGuiWindowFlags_NoSavedSettings);
        DrawPluginList(state, fixture.gui.context, 520.0F);
        ImGui::Dummy({0.0F, 12.0F});
        DrawPluginDetailPanel(state, fixture.gui.context, 680.0F, true);
        ImGui::End();
        fixture.gui.imgui.EndFrame();
    }
}  // namespace

TEST_CASE("Settings presentation renders every settings category without mutating the authority", "[unit][editor][gui][settings]") {
    using namespace Horo;
    using namespace Horo::Editor;

    SettingsPresentationFixture fixture;
    const EditorSettings committed = fixture.modal.Draft().committed;

    for (int activeTab = 0; activeTab < 9; ++activeTab) {
        DYNAMIC_SECTION("settings category " << activeTab) {
            fixture.modal.Draft().activeTab = activeTab;
            fixture.gui.imgui.BeginFrame();
            const ModalFrameResult result = fixture.modal.Draw();
            fixture.gui.imgui.EndFrame();

            REQUIRE_FALSE(result.CloseRequest().has_value());
            REQUIRE(fixture.modal.Draft().activeTab == activeTab);
            REQUIRE(CollectDraftSettings(fixture.modal.Draft()) == committed);
            REQUIRE_FALSE(fixture.modal.Draft().dirty);
        }
    }
}

TEST_CASE("Settings presentation consumes a deferred theme selection before rendering", "[unit][editor][gui][settings]") {
    using namespace Horo;
    using namespace Horo::Editor;

    SettingsPresentationFixture fixture;
    fixture.modal.Draft().appearance.pendingThemeIndex = 0;
    fixture.modal.Draft().activeTab = 1;

    fixture.gui.imgui.BeginFrame();
    static_cast<void>(fixture.modal.Draw());
    fixture.gui.imgui.EndFrame();

    REQUIRE(fixture.modal.Draft().appearance.pendingThemeIndex == -1);
}

TEST_CASE("Settings presentation renders extension and plugin detail surfaces", "[unit][editor][gui][settings]") {
    using namespace Horo::Editor;

    SettingsPresentationFixture fixture;
    for (int selectedPlugin = 0; selectedPlugin < 3; ++selectedPlugin) {
        for (int detailTab = 0; detailTab < 4; ++detailTab)
            DrawPluginWhiteBoxFrame(fixture, selectedPlugin, detailTab);
    }

    fixture.modal.Draft().activeTab = 8;
    fixture.modal.Draft().pluginSectionTab = 1;
    fixture.gui.imgui.BeginFrame();
    static_cast<void>(fixture.modal.Draw());
    fixture.gui.imgui.EndFrame();

    REQUIRE(fixture.modal.Draft().selectedPlugin == 2);
    REQUIRE(fixture.modal.Draft().pluginDetailTab[2] == 3);
}
