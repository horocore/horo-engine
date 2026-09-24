#include "Horo/Editor/GuiScreenHost.h"
#include "editor/screens/welcome/WelcomeView.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <vector>

TEST_CASE("Welcome project search and sorting preserve original project indices", "[unit][editor][welcome]") {
    using namespace Horo::Editor;
    WelcomeViewModel model;
    model.recentProjects = {{"Zulu", "/projects/alpha", "", ""}, {"Alpha", "/projects/zulu", "", ""}, {"Beta", "/projects/beta", "", ""}};
    WelcomeViewState state;
    const auto now = std::filesystem::file_time_type::clock::now();
    state.projectModifiedTimes = {now, now + std::chrono::seconds{2}, now + std::chrono::seconds{1}};
    REQUIRE((VisibleWelcomeProjectIndices(model, state) == std::vector<std::size_t>{1, 2, 0}));

    state.sortSelection = 1;
    REQUIRE((VisibleWelcomeProjectIndices(model, state) == std::vector<std::size_t>{1, 2, 0}));
    state.sortSelection = 2;
    REQUIRE((VisibleWelcomeProjectIndices(model, state) == std::vector<std::size_t>{0, 2, 1}));

    state.search[0] = 'Z';
    state.search[1] = 'U';
    state.search[2] = '\0';
    REQUIRE((VisibleWelcomeProjectIndices(model, state) == std::vector<std::size_t>{0, 1}));
}

TEST_CASE("Welcome search and sort row renders at normal and narrow widths", "[unit][editor][welcome][gui]") {
    using namespace Horo::Editor;
    Tests::EditorGuiContextFixture fixture;
    WelcomeViewModel model;
    model.recentProjects = {{"One", "/projects/one", "", ""}, {"Two", "/projects/two", "", ""}};
    WelcomeViewState state;
    for (const float width : {1280.0F, 760.0F}) {
        fixture.imgui.BeginFrame();
        const WelcomeViewResult result =
            DrawWelcomeView(model, state, fixture.context, WelcomeViewAssets{}, GuiContentRegion{0.0F, 0.0F, width, 800.0F});
        fixture.imgui.EndFrame();
        REQUIRE(result.command == WelcomeViewCommand::None);
    }
}
