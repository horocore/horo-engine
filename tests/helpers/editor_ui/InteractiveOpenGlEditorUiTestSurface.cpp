#include "EditorUiTestSurface.h"
#include "InteractiveEditorUiTestRenderer.h"
#include "editor/renderer/opengl/EditorGuiRendererOpenGL.h"
#include "editor/renderer/opengl/EditorViewportRendererOpenGL.h"
#include "editor/renderer/opengl/SdlOpenGLPresentationPort.h"
#include "runtime/renderer/modules/opengl/OpenGLBackendModule.h"

#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace Horo::Tests {
    namespace {
        [[nodiscard]] std::runtime_error MakeSdlError(const char *operation) {
            return std::runtime_error(std::string{operation} + ": " + SDL_GetError());
        }

        [[noreturn]] void ThrowRendererError(const Error &error) {
            throw std::runtime_error(error.message);
        }

        class InteractiveOpenGlEditorUiTestSurface final : public IEditorUiTestSurface {
        public:
            ~InteractiveOpenGlEditorUiTestSurface() override {
                Shutdown();
            }

            void Initialize(ImGuiContext &context) override {
                if (initialized_)
                    throw std::logic_error("Interactive OpenGL UI-test surface is already initialized.");
                ImGui::SetCurrentContext(&context);

                if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
                    throw MakeSdlError("SDL_InitSubSystem(SDL_INIT_VIDEO) failed");
                ownsVideoSubsystem_ = true;
                if (!SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) || !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24) ||
                    !SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8)) {
                    throw MakeSdlError("SDL_GL_SetAttribute failed");
                }

                window_ = SDL_CreateWindow("Horo Editor UI Test — OpenGL", 1280, 800,
                                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
                if (window_ == nullptr)
                    throw MakeSdlError("SDL_CreateWindow failed");

                presentationPort_ = std::make_unique<Editor::SdlOpenGLPresentationPort>(*window_);
                Render::RenderBackendRegistry registry;
                const Result<void> registered = Render::RegisterOpenGLRenderBackend(registry, *presentationPort_);
                if (registered.HasError())
                    ThrowRendererError(registered.ErrorValue());
                const Result<void> sealed = registry.Seal();
                if (sealed.HasError())
                    ThrowRendererError(sealed.ErrorValue());

                auto frontend = Render::RenderFrontend::Create(registry, Render::RenderBackendId{"opengl"},
                                                               Render::RenderBackendConfig{.requirePresentation = true,
                                                                                           .enableValidation = false,
                                                                                           .maxFramesInFlight = 2,
                                                                                           .presentMode = Render::PresentMode::Immediate});
                if (frontend.HasError())
                    ThrowRendererError(frontend.ErrorValue());

                std::unique_ptr<Render::RenderFrontend> frontendOwner = std::move(frontend).Value();
                auto viewportRenderer = std::make_unique<Editor::EditorViewportRendererOpenGL>(*frontendOwner);
                const Result<void> viewportInitialized = viewportRenderer->Initialize();
                if (viewportInitialized.HasError())
                    ThrowRendererError(viewportInitialized.ErrorValue());
                auto guiRenderer =
                    std::make_unique<Editor::EditorGuiRendererOpenGL>(*window_, presentationPort_->Context(), *frontendOwner);
                renderer_ =
                    InteractiveEditorUiTestRenderer::Create(std::move(frontendOwner), std::move(guiRenderer), std::move(viewportRenderer));
                initialized_ = true;
            }

            [[nodiscard]] bool BeginFrame() override {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                    if (event.type == SDL_EVENT_QUIT ||
                        (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_))) {
                        return false;
                    }
                }

                int width = 0;
                int height = 0;
                if (!SDL_GetWindowSizeInPixels(window_, &width, &height))
                    throw MakeSdlError("SDL_GetWindowSizeInPixels failed");
                if (width <= 0 || height <= 0)
                    return false;
                renderer_->BeginFrame({static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
                return true;
            }

            void Present() override {
                renderer_->Present();
            }

            void Shutdown() noexcept override {
                renderer_.reset();
                presentationPort_.reset();
                if (window_ != nullptr) {
                    SDL_DestroyWindow(window_);
                    window_ = nullptr;
                }
                if (ownsVideoSubsystem_) {
                    SDL_QuitSubSystem(SDL_INIT_VIDEO);
                    ownsVideoSubsystem_ = false;
                }
                initialized_ = false;
            }

            [[nodiscard]] bool IsInteractive() const noexcept override {
                return true;
            }

            [[nodiscard]] Editor::IEditorViewportRenderer &ViewportRenderer() noexcept override {
                return renderer_->ViewportRenderer();
            }

            void RenderViewport(const Editor::EditorViewportSceneView scene) override {
                renderer_->RenderViewport(scene);
            }

            [[nodiscard]] std::string_view RendererName() const noexcept override {
                return "opengl";
            }

        private:
            SDL_Window *window_{nullptr};
            std::unique_ptr<Editor::SdlOpenGLPresentationPort> presentationPort_;
            std::unique_ptr<InteractiveEditorUiTestRenderer> renderer_;
            bool ownsVideoSubsystem_{false};
            bool initialized_{false};
        };
    }  // namespace

    std::unique_ptr<IEditorUiTestSurface> CreateInteractiveOpenGlEditorUiTestSurface() {
        return std::make_unique<InteractiveOpenGlEditorUiTestSurface>();
    }
}  // namespace Horo::Tests
