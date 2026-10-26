/** @file GamePanelXrProjectionTests.cpp
 * @brief Actual Game panel CPU image selection; no native acquisition or renderer readiness is fabricated.
 */
#include "../../helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/screens/workspace/panels/game/GamePanel.h"

#include <catch2/catch_test_macros.hpp>
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
}  // namespace Horo::Editor::Tests
