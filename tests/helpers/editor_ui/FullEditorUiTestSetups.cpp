#include "FullEditorUiTestSetups.h"

#include "FullEditorUiTestHost.h"

#include <chrono>
#include <filesystem>
#include <imgui_test_engine/imgui_te_context.h>
#include <thread>
#include <utility>

namespace Horo::Tests::FullEditorSetups {
    void OpenProjectCreation(UiScenarioPipe &pipeline, FullEditorUiTestHost &editor) {
        pipeline.Setup("Open project creation", [&editor](ImGuiTestContext &ui) {
            ui.SetRef("Welcome");
            ui.ItemClick("**/New Project###welcome_new_project");
            for (int frame = 0; frame < 60 && editor.ActiveRoute() != Editor::GuiRouteKind::ProjectCreation; ++frame)
                ui.Yield();
            IM_CHECK(editor.ActiveRoute() == Editor::GuiRouteKind::ProjectCreation);
        });
    }

    void SubmitProjectCreation(UiScenarioPipe &pipeline, FullEditorUiTestHost &editor, FullEditorProjectSetup setup) {
        const std::filesystem::path projectRoot = editor.ProjectsRoot() / setup.name;
        pipeline.Setup("Select project template", [templateId = std::move(setup.templateId)](ImGuiTestContext &ui) {
            ui.SetRef("ProjectCreationScreen");
            ui.ItemClick(("**/###project_template_" + templateId).c_str());
            ui.ItemClick("**/Next###project_creation_next");
        });
        pipeline.Setup("Enter project identity", [name = std::move(setup.name), path = projectRoot.string()](ImGuiTestContext &ui) {
            ui.SetRef("ProjectCreationScreen");
            ui.ItemInputValue("**/###project_creation_name", name.c_str());
            ui.ItemInputValue("**/###project_creation_location", path.c_str());
            ui.ItemClick("**/Next###project_creation_next");
            ui.ItemClick("**/Next###project_creation_next");
            // MouseMove keeps the cinematic slow movement visible; MouseDown/Up
            // click without a post-click delay so the vanishing window does not
            // cause a test engine assertion.
            ui.MouseMove("**/Create###project_creation_create");
            ui.MouseDown();
            ui.MouseUp();
        });
    }

    void AwaitWorkspace(UiScenarioPipe &pipeline, FullEditorUiTestHost &editor) {
        pipeline.Setup("Wait for complete workspace", [&editor](ImGuiTestContext &ui) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            for (int frame = 0; frame < 1200 && editor.ActiveRoute() != Editor::GuiRouteKind::EditorWorkspace &&
                                std::chrono::steady_clock::now() < deadline;
                 ++frame) {
                ui.Yield();
                // Project creation intentionally advances through real-time worker stages.
                // Headless frames otherwise exhaust the deterministic frame budget first.
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            IM_CHECK(editor.WasRouteDrawn(Editor::GuiRouteKind::ProjectLoading));
            IM_CHECK(editor.ActiveRoute() == Editor::GuiRouteKind::EditorWorkspace);
        });
    }

    void CreateProjectAndOpenWorkspace(UiScenarioPipe &pipeline, FullEditorUiTestHost &editor, FullEditorProjectSetup setup) {
        OpenProjectCreation(pipeline, editor);
        SubmitProjectCreation(pipeline, editor, std::move(setup));
        AwaitWorkspace(pipeline, editor);
    }
}  // namespace Horo::Tests::FullEditorSetups
