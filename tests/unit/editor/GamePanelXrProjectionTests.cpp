/** @file GamePanelXrProjectionTests.cpp
 * @brief Actual Game panel CPU image selection; no native acquisition or renderer readiness is fabricated.
 */
#include "../../helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "Horo/AI/AISceneActivation.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/screens/workspace/panels/game/GamePanel.h"
#include "editor/screens/workspace/panels/input_mapping/InputMappingPanel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <imgui_internal.h>
#include <limits>

namespace Horo::Editor::Tests {
    namespace {
        class LogicalViewport final : public IEditorViewportRenderer {
        public:
            Result<void> ExecutePreparedFrame(Render::RenderFrameScope &, std::span<const Render::RenderPassDescriptor>) override {
                return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
            }

            Result<void> ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &) override {
                return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
            }

            void RequestExtent(EditorViewportExtent value) noexcept override {
                extent = value;
            }

            void RequestGrid(const EditorViewportGridOptions &) noexcept override {}

            void RequestLightVisualizer(const EditorViewportLightVisualizerOptions &) noexcept override {}

            EditorViewportExtent RequestedExtent() const noexcept override {
                return extent;
            }

            Math::ClipDepthRange ClipDepthRange() const noexcept override {
                return Math::ClipDepthRange::ZeroToOne;
            }

            EditorViewportTextureView TextureView() const noexcept override {
                return {.textureId = identity};
            }

            bool IsReady() const noexcept override {
                return ready;
            }

            EditorViewportExtent extent;
            std::uintptr_t identity{std::numeric_limits<std::uintptr_t>::max()};
            bool ready{};
        };

        std::size_t DrawImages(EditorGuiContextFixture &gui, GamePanel &panel, const EditorWorkspaceViewModel &model,
                               const std::uintptr_t identity, const float width = 640.0F) {
            std::size_t images{};
            for (int frame = 0; frame != 2; ++frame) {
                gui.imgui.BeginFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({width, 300});
                ImGui::Begin("Game projection", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
                EditorWorkspaceViewCommandData commands;
                panel.DrawPanel({0, 0}, ImGui::GetContentRegionAvail(), model, commands, gui.context);
                ImGui::End();
                gui.imgui.EndFrame();
                images = 0;
                REQUIRE(ImGui::GetDrawData());
                for (int list = 0; list != ImGui::GetDrawData()->CmdListsCount; ++list)
                    for (const auto &command : ImGui::GetDrawData()->CmdLists[list]->CmdBuffer)
                        if (command.ElemCount != 0 && static_cast<std::uintptr_t>(command.GetTexID()) == identity)
                            ++images;
            }
            return images;
        }
    }  // namespace

    TEST_CASE("Selected XR Game panel captures only logical identity before native acquisition", "[unit][editor][gui][xr]") {
        EditorGuiContextFixture gui;
        LogicalViewport viewport;
        GamePanel panel;
        PanelContext context{gui.editorEvents, nullptr, &viewport};
        panel.OnAttach(context);
        EditorWorkspaceViewModel model;
        model.playState = EditorPlayState::Playing;
        model.playViewSource = EditorPlayViewSource::SelectedXr;
        REQUIRE_FALSE(viewport.IsReady());
        REQUIRE_FALSE(viewport.TextureView().sampledView.IsValid());
        CHECK(DrawImages(gui, panel, model, viewport.identity, 160.0F) != 0);
        CHECK(viewport.extent.IsValid());
        CHECK_FALSE(viewport.IsReady());
        panel.OnDetach();
        CHECK(DrawImages(gui, panel, model, viewport.identity) == 0);
    }

    TEST_CASE("Authored-camera Game panel preserves physical readiness and stopped selection", "[unit][editor][gui][xr]") {
        EditorGuiContextFixture gui;
        LogicalViewport viewport;
        GamePanel panel;
        PanelContext context{gui.editorEvents, nullptr, &viewport};
        panel.OnAttach(context);
        EditorWorkspaceViewModel model;
        model.playState = EditorPlayState::Playing;
        REQUIRE(model.playViewSource == EditorPlayViewSource::AuthoredCamera);
        CHECK(DrawImages(gui, panel, model, viewport.identity) == 0);
        viewport.ready = true;
        CHECK(DrawImages(gui, panel, model, viewport.identity) != 0);
        model.playViewSource = EditorPlayViewSource::SelectedXr;
        model.playState = EditorPlayState::Stopping;
        CHECK(DrawImages(gui, panel, model, viewport.identity) == 0);
        panel.OnDetach();
    }

    TEST_CASE("Selected XR Game panel cannot draw an absent mirror identity", "[unit][editor][gui][xr]") {
        EditorGuiContextFixture gui;
        LogicalViewport viewport;
        const auto old = viewport.identity;
        viewport.identity = 0;
        GamePanel panel;
        PanelContext context{gui.editorEvents, nullptr, &viewport};
        panel.OnAttach(context);
        EditorWorkspaceViewModel model;
        model.playState = EditorPlayState::Paused;
        model.playViewSource = EditorPlayViewSource::SelectedXr;
        CHECK(DrawImages(gui, panel, model, old) == 0);
        panel.OnDetach();
    }

    TEST_CASE("Simulator Game panel hides stale texture when consumed view evidence disappears", "[unit][editor][gui][xr][simulator]") {
        EditorGuiContextFixture gui;
        LogicalViewport viewport;
        GamePanel panel;
        PanelContext context{gui.editorEvents, nullptr, &viewport};
        panel.OnAttach(context);
        EditorWorkspaceViewModel model;
        model.playState = EditorPlayState::Playing;
        model.playViewSource = EditorPlayViewSource::Simulator;
        model.simulatorPreviewAvailable = true;
        CHECK(DrawImages(gui, panel, model, viewport.identity) == 0);
        viewport.ready = true;
        CHECK(DrawImages(gui, panel, model, viewport.identity) != 0);
        model.simulatorPreviewAvailable = false;
        CHECK(DrawImages(gui, panel, model, viewport.identity) == 0);
        model.simulatorPreviewAvailable = true;
        CHECK(DrawImages(gui, panel, model, viewport.identity) != 0);
        panel.OnDetach();
    }

    TEST_CASE("Paused Game routes to the existing localized Input Mapping panel at its current dock", "[unit][editor][gui][input]") {
        EditorGuiContextFixture gui;
        const auto *locale = GENERATE("en-US", "tr-TR");
        REQUIRE(gui.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
        REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
        REQUIRE(gui.localization.ActivatePrepared());
        const auto &label = InputMappingPanel::kTextScope.Text(gui.localization, "title");
        REQUIRE(label == (std::string_view{locale} == "en-US" ? "Input Mapping" : "Girdi Eşleme"));

        GamePanel panel;
        EditorWorkspaceViewModel model;
        constexpr auto placement = WorkspaceDockArea::Left;
        const std::string panelId{InputMappingPanel::kPanelId};
        model.panelDockAreas.emplace(panelId, placement);
        const auto draw = [&](const EditorPlayState state) {
            model.playState = state;
            EditorWorkspaceViewCommandData command;
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowSize({160, 180});
            ImGui::Begin("Game pause", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            panel.DrawPanel({}, ImGui::GetContentRegionAvail(), model, command, gui.context);
            ImGui::End();
            gui.imgui.EndFrame();
            return command;
        };

        static_cast<void>(draw(EditorPlayState::Paused));
        ImGuiWindow *content = nullptr;
        for (ImGuiWindow *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && window->ParentWindow && std::string_view{window->Name}.find("##GameContent") != std::string_view::npos)
                content = window;
        REQUIRE(content != nullptr);
        ImGui::FocusWindow(content);

        ImGui::ActivateItemByID(content->GetID(label.c_str()));
        const auto playing = draw(EditorPlayState::Playing);
        CHECK(playing.command == EditorWorkspaceViewCommand::None);
        ImGui::ActivateItemByID(content->GetID(label.c_str()));
        const auto paused = draw(EditorPlayState::Paused);
        CHECK(paused.command == EditorWorkspaceViewCommand::ChangeActivePanel);
        CHECK(paused.targetIndex == static_cast<int>(placement));
        CHECK(paused.stringPayload == panelId);
    }

    TEST_CASE("Game panel opens read-only AI inspection and clears it when the selected agent expires", "[unit][editor][gui][ai]") {
        EditorGuiContextFixture gui;
        const auto *locale = GENERATE("en-US", "tr-TR");
        REQUIRE(gui.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
        REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
        REQUIRE(gui.localization.ActivatePrepared());
        const auto &label = GamePanel::kTextScope.Text(gui.localization, "ai_inspection_title");
        REQUIRE(label == (std::string_view{locale} == "en-US" ? "AI agent inspector" : "AI ajan denetleyicisi"));

        GamePanel panel;
        EditorWorkspaceViewModel model;
        model.playState = EditorPlayState::Playing;
        const AI::AgentHandle handle{AI::AiRuntimeIncarnation::Create(7).Value(), {0, 1}};
        const auto key = AI::BlackboardKeyId::Create(1).Value();
        for (const std::uint64_t tick : {1, 5}) {
            auto snapshot = std::make_shared<AI::AiAgentInspectionSnapshot>();
            snapshot->agent = handle;
            snapshot->decisionTick = tick;
            snapshot->blackboardRevision = tick;
            snapshot->keys.push_back({key,
                                      AI::BlackboardValueKind::SignedInteger,
                                      AI::BlackboardValueCardinality::Scalar,
                                      AI::AiInspectionValueStatus::Redacted,
                                      {}});
            model.playAgentInspection.push_back({{}, handle, tick, std::move(snapshot)});
        }
        const auto draw = [&] {
            EditorWorkspaceViewCommandData command;
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowSize({190, 300});
            ImGui::Begin("Game AI", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            panel.DrawPanel({}, ImGui::GetContentRegionAvail(), model, command, gui.context);
            ImGui::End();
            gui.imgui.EndFrame();
            return command.command;
        };
        const auto find = [](const std::string_view suffix) {
            for (auto *window : ImGui::GetCurrentContext()->Windows)
                if (window->Active && window->ParentWindow && std::string_view{window->Name}.find(suffix) != std::string_view::npos)
                    return window;
            return static_cast<ImGuiWindow *>(nullptr);
        };
        CHECK(draw() == EditorWorkspaceViewCommand::None);
        const auto content = find("##GameContent");
        REQUIRE(content);
        CHECK_FALSE(find("##AgentInspection"));
        ImGui::FocusWindow(content);
        ImGui::ActivateItemByID(content->GetID(label.c_str()));
        CHECK(draw() == EditorWorkspaceViewCommand::None);
        REQUIRE(find("##AgentInspection"));
        model.playAgentInspection.clear();
        CHECK(draw() == EditorWorkspaceViewCommand::None);
        CHECK_FALSE(find("##AgentInspection"));
    }
}  // namespace Horo::Editor::Tests
