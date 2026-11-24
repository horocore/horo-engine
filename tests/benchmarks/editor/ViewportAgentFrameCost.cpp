/** @file
 * @brief Opt-in CPU measurement of the editor viewport when the agent is disabled.
 */
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Foundation/DataBus.h"
#include "ViewportPanelRenderSupport.h"
#include "editor/screens/workspace/panels/viewport/ViewportPanel.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <imgui.h>

namespace {
    using namespace Horo::Editor;

    /** @brief Times only the panel draw within a complete, settled ImGui frame. */
    std::chrono::steady_clock::duration DrawViewportFrame(ViewportPanel &panel, const EditorWorkspaceViewModel &viewModel,
                                                          EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({840.0F, 560.0F});
        ImGui::Begin("ViewportAgentFrameCost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const auto start = std::chrono::steady_clock::now();
        panel.DrawPanel(ImGui::GetCursorScreenPos(), {800.0F, 500.0F}, viewModel, command, context);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        ImGui::End();
        ImGui::Render();
        return elapsed;
    }
}  // namespace

TEST_CASE("Measure disabled viewport agent panel cost", "[.][viewport-agent-frame-cost]") {
    using namespace Horo;
    using namespace Horo::Editor;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {960.0F, 640.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());

    EngineDataBus engineEvents;
    EditorDataBus editorEvents;
    Horo::Editor::Tests::ViewportRenderSupport::TestLocalization localization;
    const auto font = io.Fonts->Fonts.front();
    const ThemeContext theme{.fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font}};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engineEvents,
                                   .editorEvents = editorEvents,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};
    EditorWorkspaceViewModel viewModel;
    viewModel.objects.resize(4'096);
    EditorWorkspaceViewCommandData command;
    Horo::Editor::Tests::ViewportRenderSupport::FakeViewportRenderer renderer;
    ViewportPanel panel;
    PanelContext panelContext{.dataBus = editorEvents, .viewportRenderer = &renderer};
    panel.OnAttach(panelContext);

    for (int frame = 0; frame < 100; ++frame)
        DrawViewportFrame(panel, viewModel, command, context);
    constexpr int Samples = 10'000;
    std::chrono::steady_clock::duration elapsed{};
    for (int frame = 0; frame < Samples; ++frame)
        elapsed += DrawViewportFrame(panel, viewModel, command, context);
    const auto vertices = ImGui::GetDrawData()->TotalVtxCount;
    const auto indices = ImGui::GetDrawData()->TotalIdxCount;
    panel.OnDetach();
    ImGui::DestroyContext();

    const double nanosecondsPerFrame = std::chrono::duration<double, std::nano>(elapsed).count() / Samples;
    INFO(nanosecondsPerFrame << " ns/frame, " << vertices << " vertices, " << indices << " indices");
    CHECK(vertices > 0);
    CHECK(command.command == EditorWorkspaceViewCommand::None);
}
