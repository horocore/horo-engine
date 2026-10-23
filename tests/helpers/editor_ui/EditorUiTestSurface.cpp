#include "EditorUiTestSurface.h"

#include "Horo/Runtime/Render/RenderFrontend.h"

#include <imgui.h>
#include <stdexcept>
#include <string_view>

namespace Horo::Tests {
    namespace {
        class NullViewportRenderer final : public Editor::IEditorViewportRenderer {
        public:
            void RequestExtent(const Editor::EditorViewportExtent extent) noexcept override {
                extent_ = extent;
            }

            void RequestGrid(const Editor::EditorViewportGridOptions &) noexcept override {}

            void RequestLightVisualizer(const Editor::EditorViewportLightVisualizerOptions &) noexcept override {}

            [[nodiscard]] Editor::EditorViewportExtent RequestedExtent() const noexcept override {
                return extent_;
            }

            [[nodiscard]] Math::ClipDepthRange ClipDepthRange() const noexcept override {
                return Math::ClipDepthRange::NegativeOneToOne;
            }

            [[nodiscard]] Horo::Result<void> ExecutePreparedFrame(Horo::Render::RenderFrameScope &frame,
                                                                  std::span<const Horo::Render::RenderPassDescriptor> passes) override {
                return frame.Execute(passes);
            }

            [[nodiscard]] Result<void> ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &) override {
                return Result<void>::Success();
            }

            [[nodiscard]] Editor::EditorViewportTextureView TextureView() const noexcept override {
                return {};
            }

            [[nodiscard]] bool IsReady() const noexcept override {
                return false;
            }

        private:
            Editor::EditorViewportExtent extent_{};
        };

        class HeadlessEditorUiTestSurface final : public IEditorUiTestSurface {
        public:
            void Initialize(ImGuiContext &context) override {
                ImGui::SetCurrentContext(&context);
                ImGuiIO &io = ImGui::GetIO();
                io.DisplaySize = {1280.0F, 800.0F};
                io.DisplayFramebufferScale = {1.0F, 1.0F};
                io.DeltaTime = 1.0F / 60.0F;
            }

            [[nodiscard]] bool BeginFrame() override {
                return true;
            }

            void Present() override {}

            void Shutdown() noexcept override {}

            [[nodiscard]] bool IsInteractive() const noexcept override {
                return false;
            }

            [[nodiscard]] Editor::IEditorViewportRenderer &ViewportRenderer() noexcept override {
                return viewportRenderer_;
            }

            void RenderViewport(const Editor::EditorViewportSceneView) override {}

            [[nodiscard]] std::string_view RendererName() const noexcept override {
                return "null";
            }

        private:
            NullViewportRenderer viewportRenderer_;
        };
    }  // namespace

    std::unique_ptr<IEditorUiTestSurface> CreateHeadlessEditorUiTestSurface() {
        return std::make_unique<HeadlessEditorUiTestSurface>();
    }

#if !defined(HORO_UI_TEST_HAS_INTERACTIVE_OPENGL)
    std::unique_ptr<IEditorUiTestSurface> CreateInteractiveOpenGlEditorUiTestSurface() {
        throw std::runtime_error("Interactive UI tests require a test-enabled build with HORO_BUILD_RENDER_OPENGL=ON.");
    }
#endif

#if !defined(HORO_UI_TEST_HAS_INTERACTIVE_METAL)
    std::unique_ptr<IEditorUiTestSurface> CreateInteractiveMetalEditorUiTestSurface() {
        throw std::runtime_error("Interactive UI tests require a test-enabled build with HORO_BUILD_RENDER_METAL=ON.");
    }
#endif

    std::unique_ptr<IEditorUiTestSurface> CreateEditorUiTestSurfaceFromEnvironment() {
        if (const char *const legacy = std::getenv("HORO_UI_TEST_PRESENTATION"); legacy != nullptr && legacy[0] != '\0') {
            throw std::invalid_argument(
                "HORO_UI_TEST_PRESENTATION was replaced by HORO_UI_TEST_RENDERER; use 'null', 'opengl', or 'metal'.");
        }

        const char *const requested = std::getenv("HORO_UI_TEST_RENDERER");
        if (requested == nullptr || requested[0] == '\0' || std::string_view{requested} == "null")
            return CreateHeadlessEditorUiTestSurface();
        if (std::string_view{requested} == "opengl")
            return CreateInteractiveOpenGlEditorUiTestSurface();
        if (std::string_view{requested} == "metal")
            return CreateInteractiveMetalEditorUiTestSurface();
        throw std::invalid_argument("HORO_UI_TEST_RENDERER must be 'null', 'opengl', or 'metal'.");
    }
}  // namespace Horo::Tests
