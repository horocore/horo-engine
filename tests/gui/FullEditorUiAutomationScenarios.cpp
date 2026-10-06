#include "FullEditorUiTestActions.h"
#include "FullEditorUiTestHost.h"
#include "FullEditorUiTestSetups.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "editor/renderer/EditorViewportScene.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_test_engine/imgui_te_context.h>
#include <string>
#include <string_view>
#include <thread>

namespace {
    using namespace Horo;

    enum class GameplayRuntimeKind {
        Lua,
        Native
    };

    enum class GameplayJourneyExtent {
        AttachmentOnly,
        FullPlay
    };

    /** @brief Keeps attachment qualification distinct from the complete native/script Play scenarios. */
    const char *GameplayJourneyName(const GameplayRuntimeKind runtime, const GameplayJourneyExtent extent) {
        if (extent == GameplayJourneyExtent::AttachmentOnly)
            return "native_behavior_attachment_journey";
        return runtime == GameplayRuntimeKind::Lua ? "lua_behavior_play_journey" : "native_behavior_play_journey";
    }

    std::string GameplayBehaviorTypeId(const std::filesystem::path &projectRoot, const GameplayRuntimeKind runtime) {
        std::string projectNamespace = projectRoot.filename().string();
        std::ranges::transform(projectNamespace, projectNamespace.begin(), [](const unsigned char value) {
            return std::isalnum(value) ? static_cast<char>(std::tolower(value)) : '_';
        });
        return "game." + projectNamespace + (runtime == GameplayRuntimeKind::Lua ? ".lua.newbehavior" : ".cpp.newbehavior");
    }

    void InstallMovementFixture(const std::filesystem::path &projectRoot, const GameplayRuntimeKind runtime) {
        const std::string typeId = GameplayBehaviorTypeId(projectRoot, runtime);
        const std::filesystem::path fixtureRoot = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/gameplay_e2e";
        const std::filesystem::path fixture =
            fixtureRoot / (runtime == GameplayRuntimeKind::Lua ? "SemanticInputMovement.horo_script" : "SemanticInputMovement.cpp");
        const std::filesystem::path destination = runtime == GameplayRuntimeKind::Lua
                                                      ? projectRoot / "Assets/Scripts/NewBehavior.horo_script"
                                                      : projectRoot / "source/gameplay/NewBehavior.cpp";
        std::ifstream input{fixture, std::ios::binary};
        std::string contents{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
        constexpr std::string_view token = "{{BEHAVIOR_TYPE_ID}}";
        for (std::size_t position = contents.find(token); position != std::string::npos; position = contents.find(token, position)) {
            contents.replace(position, token.size(), typeId);
            position += typeId.size();
        }
        std::ofstream output{destination, std::ios::binary | std::ios::trunc};
        output << contents;
        if (runtime == GameplayRuntimeKind::Lua) {
            std::ifstream metadataInput{fixture.string() + ".meta", std::ios::binary};
            std::string metadata{std::istreambuf_iterator{metadataInput}, std::istreambuf_iterator<char>{}};
            if (const std::size_t position = metadata.find(token); position != std::string::npos)
                metadata.replace(position, token.size(), typeId);
            std::ofstream metadataOutput{destination.string() + ".meta", std::ios::binary | std::ios::trunc};
            metadataOutput << metadata;
        }
    }

    /** @brief Creates the active camera and restores the authored object selection before component attachment. */
    void AddCameraSelectionStep(Tests::UiScenarioPipe &pipeline) {
        pipeline.Step("Create a Camera and reselect the authored Box", [](ImGuiTestContext &ui) {
            ui.ItemClick("**/##HierarchyRootDrop", ImGuiMouseButton_Right);
            ui.SetRef("//$FOCUSED");
            ui.ItemClick("###hierarchy_create_root");
            ui.Yield();
            ui.ItemClick("//**/###hierarchy_create_workspace.create.group.cameras");
            ui.Yield();
            ui.ItemClick("//**/###hierarchy_create_primitive.object.camera");
            ui.ItemClick("//**/##hierarchy_object_row");
            ui.Yield();
            if (!ui.ItemExists("//**/###InspectorAddComponent")) {
                ui.ItemClick("//**/horo.inspector/##ActivityItem");
                ui.Yield();
            }
            if (!ui.ItemExists("//**/###InspectorAddComponent")) {
                ui.ItemClick("//**/##hierarchy_object_row");
                ui.Yield();
            }
            IM_CHECK(ui.ItemExists("//**/###InspectorAddComponent"));
        });
    }

    /** @brief Creates the requested native or script behavior through the real Asset Browser route. */
    void AddBehaviorCreationStep(Tests::UiScenarioPipe &pipeline, const GameplayRuntimeKind runtime) {
        pipeline.Step("Create behavior through the Asset Browser", [runtime](ImGuiTestContext &ui) {
            if (!ui.ItemExists("//**/Assets")) {
                ui.ItemClick("//**/horo.global_dock/##ActivityItem");
                ui.Yield();
            }
            ui.ItemClick("//**/Assets");
            ImGuiTestItemInfo dock = ui.WindowInfo("//##DockBottom", ImGuiTestOpFlags_NoError);
            if (dock.Window == nullptr)
                dock = ui.WindowInfo("//##DockBottomLeft", ImGuiTestOpFlags_NoError);
            if (dock.Window == nullptr)
                dock = ui.WindowInfo("//##DockBottomRight", ImGuiTestOpFlags_NoError);
            IM_CHECK(dock.Window != nullptr);
            ui.MouseMoveToPos({dock.RectClipped.Max.x - 20.0F, dock.RectClipped.GetCenter().y});
            ui.MouseClick(ImGuiMouseButton_Right);
            ui.ItemClick(runtime == GameplayRuntimeKind::Lua ? "//**/###content_browser_create_lua_behavior"
                                                             : "//**/###content_browser_create_native_behavior");
            ui.Yield();
            ui.ItemInputValue("//**/##GameplayBehaviorFilename", "NewBehavior");
            ui.ItemClick("//**/Create");
        });
    }

    /** @brief Replaces the generated behavior with the shared versioned semantic-input fixture. */
    void AddMovementFixtureStep(Tests::UiScenarioPipe &pipeline, const std::filesystem::path &projectRoot,
                                const GameplayRuntimeKind runtime) {
        pipeline.Step("Install the versioned semantic-input fixture", [projectRoot, runtime](ImGuiTestContext &ui) {
            const std::filesystem::path generated = runtime == GameplayRuntimeKind::Lua
                                                        ? projectRoot / "Assets/Scripts/NewBehavior.horo_script"
                                                        : projectRoot / "source/gameplay/NewBehavior.cpp";
            for (int frame = 0; frame < 30 && !std::filesystem::is_regular_file(generated); ++frame)
                ui.Yield();
            if (!std::filesystem::is_regular_file(generated)) {
                for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(projectRoot))
                    ui.LogInfo("Generated project file: %s", entry.path().string().c_str());
            }
            IM_CHECK(std::filesystem::is_regular_file(generated));
            InstallMovementFixture(projectRoot, runtime);
        });
    }

    /** @brief Waits for current build and catalog evidence before checking attachment and duplicate exclusion. */
    void AddBehaviorAttachmentStep(Tests::UiScenarioPipe &pipeline, Tests::FullEditorUiTestHost &editor,
                                   const std::filesystem::path &projectRoot, const std::string &behaviorTypeId,
                                   const GameplayRuntimeKind runtime) {
        pipeline.Step("Attach behavior through Inspector", [projectRoot, runtime, behaviorTypeId, &editor](ImGuiTestContext &ui) {
            const std::uint64_t beforeAttachment = editor.ViewportDocumentRevision();
            const std::string behaviorItem = "//**/###inspector_behavior_" + behaviorTypeId;
            ui.ItemClick("//**/###InspectorAddComponent");
            bool ready = false;
            for (int frame = 0; frame < 1500; ++frame) {
                const bool currentInputs = runtime == GameplayRuntimeKind::Lua || editor.IsGameplayBuildUpToDate(projectRoot);
                ready = currentInputs && ui.ItemExists(behaviorItem.c_str());
                if (ready)
                    break;
                ui.Yield();
                if (runtime == GameplayRuntimeKind::Native)
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
            if (!ready)
                ui.LogInfo("Gameplay catalog did not advertise '%s' for current inputs. Build output:\n%s", behaviorTypeId.c_str(),
                           editor.BuildDiagnosticText().c_str());
            IM_CHECK(ready);
            ui.ItemClick(behaviorItem.c_str());
            ui.Yield();
            IM_CHECK(editor.ViewportDocumentRevision() > beforeAttachment);
            ui.ItemClick("//**/###InspectorAddComponent");
            IM_CHECK(!ui.ItemExists(behaviorItem.c_str()));
            ui.KeyPress(ImGuiKey_Escape);
        });
    }

    /** @brief Checks movement, pause, stepping and restoration of the original authoring state. */
    void AddSemanticInputPlayStep(Tests::UiScenarioPipe &pipeline, Tests::FullEditorUiTestHost &editor) {
        pipeline.Step("Play one exact semantic-input tick", [&editor](ImGuiTestContext &ui) {
            for (int frame = 0; frame < 30 && !editor.FirstViewportObjectPosition(); ++frame)
                ui.Yield();
            const auto before = editor.FirstViewportObjectPosition();
            IM_CHECK(before.has_value());
            const std::uint64_t authoringRevision = editor.ViewportDocumentRevision();
            ui.ItemClick("//**/###workspace_play_play");
            for (int frame = 0; frame < 120; ++frame) {
                const ImGuiTestItemInfo pause = ui.ItemInfo("//**/###workspace_play_pause", ImGuiTestOpFlags_NoError);
                if (pause.ID != 0 && (pause.ItemFlags & ImGuiItemFlags_Disabled) == 0)
                    break;
                ui.Yield();
            }
            ui.KeyDown(ImGuiKey_RightArrow);
            ui.Yield();
            editor.AdvanceFixedTicks(1);
            ui.Yield();
            IM_CHECK(ui.ItemExists("//**/###workspace_play_pause"));
            const auto moved = editor.FirstViewportObjectPosition();
            IM_CHECK(moved.has_value());
            IM_CHECK(moved->x == before->x + 1.0F);

            ui.ItemClick("//**/###workspace_play_pause");
            editor.AdvanceFixedTicks(1);
            IM_CHECK(editor.FirstViewportObjectPosition()->x == moved->x);
            ui.ItemClick("//**/###workspace_play_step");
            editor.AdvanceFixedTicks(1);
            IM_CHECK(editor.FirstViewportObjectPosition()->x == moved->x + 1.0F);
            ui.KeyUp(ImGuiKey_RightArrow);
            ui.ItemClick("//**/###workspace_play_stop");
            ui.Yield();
            IM_CHECK(editor.ViewportDocumentRevision() == authoringRevision);
            IM_CHECK(editor.FirstViewportObjectPosition()->x == before->x);
        });
    }

    Tests::EditorUiScenarioResult RunBehaviorPlayJourney(const GameplayRuntimeKind runtime,
                                                         const GameplayJourneyExtent extent = GameplayJourneyExtent::FullPlay) {
        Tests::EditorUiTestHarness harness;
        Tests::FullEditorUiTestHost editor{harness.Surface(), "en-US"};
        const std::string projectName = runtime == GameplayRuntimeKind::Lua ? "LuaGameplayJourney" : "NativeGameplayJourney";
        const std::filesystem::path projectRoot = editor.ProjectsRoot() / projectName;
        const std::string behaviorTypeId = GameplayBehaviorTypeId(projectRoot, runtime);
        return harness.RunScenario("gameplay", GameplayJourneyName(runtime, extent), [&editor](ImGuiTestContext *context) {
            editor.DrawFrame(context);
        }, [&editor, projectRoot, projectName, behaviorTypeId, runtime, extent](Tests::UiScenarioPipe &pipeline) {
            Tests::FullEditorSetups::CreateProjectAndOpenWorkspace(pipeline, editor, {.name = projectName, .templateId = "3d-starter"});
            Tests::FullEditorActions::CreateRootBox(pipeline);
            AddCameraSelectionStep(pipeline);
            AddBehaviorCreationStep(pipeline, runtime);
            AddMovementFixtureStep(pipeline, projectRoot, runtime);
            AddBehaviorAttachmentStep(pipeline, editor, projectRoot, behaviorTypeId, runtime);
            if (extent == GameplayJourneyExtent::AttachmentOnly)
                return;
            AddSemanticInputPlayStep(pipeline, editor);
        }, Tests::EditorUiScenarioBudget::Extended(6000), &editor.Input());
    }

    TEST_CASE("Selected E2E renderer produces an editor viewport texture", "[ui][imgui][editor][e2e][renderer]") {
        Tests::EditorUiTestHarness harness;
        Tests::IEditorUiTestSurface &surface = harness.Surface();
        CAPTURE(std::string{surface.RendererName()});

        Runtime::PrimitiveMeshCache meshCache;
        auto acquired = meshCache.Acquire(Runtime::PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box));
        REQUIRE(acquired.HasValue());
        Runtime::PrimitiveMeshLease meshLease = std::move(acquired).Value();
        const Render::MeshData &mesh = meshLease.Data();
        const Render::RenderMeshSourceHandle meshHandle{meshLease.Id(), 1};
        const std::array meshResources{Editor::EditorViewportMeshResourceView{meshHandle, mesh.vertices, mesh.indices, mesh.localBounds}};
        const std::array instances{Editor::EditorViewportInstance{meshHandle,
                                                                  Math::Transform{}.ToMatrix(),
                                                                  mesh.localBounds,
                                                                  Render::CoreDefaultMaterial,
                                                                  {.tint = {0.12F, 0.72F, 1.0F}, .tintStrength = 0.65F}}};
        const Editor::EditorViewportSceneView scene{.camera = {}, .meshResources = meshResources, .instances = instances};

        const Tests::EditorUiScenarioResult result =
            harness.Run("renderer_e2e", "selected_renderer_viewport", [&surface, scene](ImGuiTestContext *) {
            constexpr Editor::EditorViewportExtent extent{640, 360};
            surface.ViewportRenderer().RequestExtent(extent);
            surface.RenderViewport(scene);

            ImGui::SetNextWindowSize({680.0F, 420.0F}, ImGuiCond_Always);
            ImGui::Begin("Renderer E2E", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
            if (const Editor::EditorViewportTextureView texture = surface.ViewportRenderer().TextureView(); texture.IsValid()) {
                ImGui::Image(texture.textureId, {640.0F, 360.0F}, {texture.u0, texture.v0}, {texture.u1, texture.v1});
            } else {
                ImGui::TextUnformatted("Waiting for selected renderer...");
            }
            ImGui::End();
        }, [&surface](ImGuiTestContext *context) {
            const bool expectsTexture = surface.RendererName() != "null";
            for (int frame = 0; frame < 120 && expectsTexture && !surface.ViewportRenderer().IsReady(); ++frame)
                context->Yield();
            if (expectsTexture) {
                IM_CHECK(surface.ViewportRenderer().IsReady());
                IM_CHECK(surface.ViewportRenderer().TextureView().IsValid());
            }
        });

        INFO(result.testEngineLog);
        REQUIRE_FALSE(result.frameBudgetExceeded);
        REQUIRE_FALSE(result.cancelled);
        REQUIRE(result.exception == nullptr);
        REQUIRE(result.Succeeded());
        if (surface.RendererName() != "null")
            REQUIRE(surface.ViewportRenderer().IsReady());
    }

    TEST_CASE("Full editor project journey creates content and configures the viewport", "[ui][imgui][editor][e2e]") {
        const char *const envLocale = std::getenv("HORO_UI_TEST_LOCALE");
        const std::string locale = (envLocale != nullptr && envLocale[0] != '\0') ? std::string{envLocale} : std::string{"en-US"};
        CAPTURE(locale);
        Tests::EditorUiTestHarness harness;
        Tests::FullEditorUiTestHost editor{harness.Surface(), locale};
        CAPTURE(std::string{editor.RendererName()});
        const Tests::FullEditorProjectSetup project{.name = "ProjectJourney", .templateId = "3d-starter"};
        const std::filesystem::path projectRoot = editor.ProjectsRoot() / project.name;

        const Tests::EditorUiScenarioResult result =
            harness.RunScenario("full_editor", "project_journey_" + locale, [&editor](ImGuiTestContext *context) {
            editor.DrawFrame(context);
        }, [&editor, project](Tests::UiScenarioPipe &pipeline) {
            Tests::FullEditorSetups::CreateProjectAndOpenWorkspace(pipeline, editor, project);
            Tests::FullEditorActions::CreateRootBox(pipeline);
            Tests::FullEditorActions::ExerciseHierarchyEdits(pipeline);
            Tests::FullEditorActions::ExerciseInspectorComponents(pipeline);
            Tests::FullEditorActions::SelectOrthographicProjection(pipeline, editor);
            Tests::FullEditorActions::ExerciseWorkspacePanels(pipeline, editor);
            Tests::FullEditorActions::ExerciseAssetImport(pipeline, editor);
        }, Tests::EditorUiScenarioBudget::Extended(2400), &editor.Input());

        INFO(result.testEngineLog);
        REQUIRE_FALSE(result.frameBudgetExceeded);
        REQUIRE_FALSE(result.cancelled);
        REQUIRE(result.exception == nullptr);
        REQUIRE(result.Succeeded());
        REQUIRE(result.steps.size() == 17);
        REQUIRE(std::all_of(result.steps.begin(), result.steps.begin() + 4, [](const Tests::UiScenarioStepResult &step) {
            return step.kind == Tests::UiScenarioStepKind::Setup && step.status == Tests::UiScenarioStepStatus::Passed;
        }));
        REQUIRE(std::all_of(result.steps.begin() + 4, result.steps.end(), [](const Tests::UiScenarioStepResult &step) {
            return step.kind == Tests::UiScenarioStepKind::Test && step.status == Tests::UiScenarioStepStatus::Passed;
        }));
        REQUIRE(std::filesystem::exists(projectRoot / ".horo" / "project.json"));
        REQUIRE(editor.ViewportProjection() == Runtime::CameraProjection::Orthographic);
        if (editor.RendererName() != "null")
            REQUIRE(editor.RendererReady());
    }

    TEST_CASE("Empty project opens without a configured default scene", "[ui][imgui][editor][e2e][project-creation]") {
        Tests::EditorUiTestHarness harness;
        Tests::FullEditorUiTestHost editor{harness.Surface(), "en-US"};
        const Tests::FullEditorProjectSetup project{.name = "EmptyProjectJourney", .templateId = "empty"};
        const std::filesystem::path projectRoot = editor.ProjectsRoot() / project.name;

        const Tests::EditorUiScenarioResult result =
            harness.RunScenario("full_editor", "empty_project_journey", [&editor](ImGuiTestContext *context) {
            editor.DrawFrame(context);
        }, [&editor, project](Tests::UiScenarioPipe &pipeline) {
            Tests::FullEditorSetups::CreateProjectAndOpenWorkspace(pipeline, editor, project);
        }, Tests::EditorUiScenarioBudget::Extended(1400), &editor.Input());

        INFO(result.testEngineLog);
        REQUIRE_FALSE(result.frameBudgetExceeded);
        REQUIRE_FALSE(result.cancelled);
        REQUIRE(result.exception == nullptr);
        REQUIRE(result.Succeeded());
        REQUIRE(editor.ActiveRoute() == Editor::GuiRouteKind::EditorWorkspace);
        REQUIRE(std::filesystem::is_empty(projectRoot / "Assets/Scenes"));
        std::ifstream metadataInput{projectRoot / ".horo/project.json", std::ios::binary};
        const std::string metadata{std::istreambuf_iterator<char>{metadataInput}, std::istreambuf_iterator<char>{}};
        REQUIRE((metadata.find("\"defaultScene\": \"\"") != std::string::npos));
    }

    TEST_CASE("Lua behavior completes the editor Play journey", "[ui][imgui][editor][e2e][gameplay][lua]") {
        const Tests::EditorUiScenarioResult result = RunBehaviorPlayJourney(GameplayRuntimeKind::Lua);
        INFO(result.testEngineLog);
        REQUIRE_FALSE(result.frameBudgetExceeded);
        REQUIRE_FALSE(result.cancelled);
        REQUIRE(result.exception == nullptr);
        REQUIRE(result.Succeeded());
    }

    TEST_CASE("Native behavior attachment waits for the current artifact and advertised catalog",
              "[ui][imgui][editor][e2e][gameplay][native]") {
        const Tests::EditorUiScenarioResult result =
            RunBehaviorPlayJourney(GameplayRuntimeKind::Native, GameplayJourneyExtent::AttachmentOnly);
        INFO(result.testEngineLog);
        REQUIRE_FALSE(result.frameBudgetExceeded);
        REQUIRE_FALSE(result.cancelled);
        REQUIRE(result.exception == nullptr);
        REQUIRE(result.Succeeded());
    }

    TEST_CASE("Native behavior completes the editor Play journey", "[ui][imgui][editor][e2e][gameplay][native]") {
        const Tests::EditorUiScenarioResult result = RunBehaviorPlayJourney(GameplayRuntimeKind::Native);
        INFO(result.testEngineLog);
        REQUIRE_FALSE(result.frameBudgetExceeded);
        REQUIRE_FALSE(result.cancelled);
        REQUIRE(result.exception == nullptr);
        REQUIRE(result.Succeeded());
    }

    TEST_CASE("Recent project opens through the loading screen", "[ui][imgui][editor][e2e][recent-project]") {
        Tests::EditorUiTestHarness harness;
        Tests::FullEditorUiTestHost editor{harness.Surface(), "en-US", "RecentProject"};
        const std::filesystem::path projectRoot = editor.ProjectsRoot() / "RecentProject";

        const Tests::EditorUiScenarioResult result =
            harness.RunScenario("full_editor", "open_recent_project", [&editor](ImGuiTestContext *context) {
            editor.DrawFrame(context);
        }, [&editor](Tests::UiScenarioPipe &pipeline) {
            pipeline.Setup("Open recent project", [](ImGuiTestContext &ui) {
                ui.SetRef("Welcome");
                ui.ItemClick("**/Project card###welcome_project_card");
            });
            Tests::FullEditorSetups::AwaitWorkspace(pipeline, editor);
        }, Tests::EditorUiScenarioBudget::Extended(1400), &editor.Input());

        INFO(result.testEngineLog);
        REQUIRE_FALSE(result.frameBudgetExceeded);
        REQUIRE_FALSE(result.cancelled);
        REQUIRE(result.exception == nullptr);
        REQUIRE(result.Succeeded());
        REQUIRE(editor.WasRouteDrawn(Editor::GuiRouteKind::ProjectLoading));
        REQUIRE(editor.RouteDrawCount(Editor::GuiRouteKind::ProjectLoading) >= 2);
        REQUIRE(editor.ActiveRoute() == Editor::GuiRouteKind::EditorWorkspace);
        REQUIRE(std::filesystem::exists(projectRoot / ".horo" / "project.json"));
    }
}  // namespace
