#include "EditorGuiRendererOpenGL.h"

#include "editor/renderer/EditorRenderMemoryScopes.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/renderer/opengl/OpenGLViewportResourceBridge.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <string>

namespace Horo::Editor {
    namespace {
        /** @brief Selects the primary backbuffer and restores its draw selection plus caller state. */
        class GuiOutputScope final {
        public:
            GuiOutputScope() noexcept {
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer_);
                glGetBooleani_v(GL_COLOR_WRITEMASK, 0, colorMask_.data());
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                glGetIntegerv(GL_DRAW_BUFFER, &drawBuffer_);
                glDrawBuffer(GL_BACK);
                glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }

            ~GuiOutputScope() {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                glDrawBuffer(static_cast<GLenum>(drawBuffer_));
                glColorMaski(0, colorMask_[0], colorMask_[1], colorMask_[2], colorMask_[3]);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer_));
            }

            GuiOutputScope(const GuiOutputScope &) = delete;
            GuiOutputScope &operator=(const GuiOutputScope &) = delete;

        private:
            GLint drawFramebuffer_{0};
            GLint drawBuffer_{GL_BACK};
            std::array<GLboolean, 4> colorMask_{};
        };

        [[nodiscard]] Error MakeGuiRendererError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }
    }  // namespace

    /** @copydoc EditorGuiRendererOpenGL::EditorGuiRendererOpenGL */
    EditorGuiRendererOpenGL::EditorGuiRendererOpenGL(SDL_Window &window, const SDL_GLContext context,
                                                     Render::RenderFrontend &frontend) noexcept
        : window_(&window), context_(context), frontend_(&frontend) {}

    /** @copydoc EditorGuiRendererOpenGL::~EditorGuiRendererOpenGL */
    EditorGuiRendererOpenGL::~EditorGuiRendererOpenGL() {
        Shutdown();
    }

    /** @copydoc EditorGuiRendererOpenGL::ValidateContext */
    Result<void> EditorGuiRendererOpenGL::ValidateContext() const {
        if (ownerThread_ != std::this_thread::get_id() || ImGui::GetCurrentContext() == nullptr || context_ == nullptr ||
            SDL_GL_GetCurrentContext() != context_ || SDL_GL_GetCurrentWindow() != window_) {
            return Result<void>::Failure(
                MakeGuiRendererError(RendererErrors::GuiInvalidState,
                                     "OpenGL GUI work requires its current window/context on the creating thread."));
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererOpenGL::Initialize */
    Result<void> EditorGuiRendererOpenGL::Initialize() {
        if (const auto current = ValidateContext(); current.HasError())
            return current;
        if (platformInitialized_ || rendererInitialized_ || context_ == nullptr) {
            return Result<void>::Failure(
                MakeGuiRendererError(RendererErrors::GuiInvalidState, "OpenGL GUI renderer state or context is invalid."));
        }
        platformInitialized_ = ImGui_ImplSDL3_InitForOpenGL(window_, context_);
        rendererInitialized_ = platformInitialized_ && ImGui_ImplOpenGL3_Init("#version 150");
        if (!rendererInitialized_) {
            Shutdown();
            return Result<void>::Failure(
                MakeGuiRendererError(RendererErrors::GuiInitializationFailed, "Failed to initialize Dear ImGui SDL3/OpenGL bridges."));
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererOpenGL::BeginFrame */
    Result<void> EditorGuiRendererOpenGL::BeginFrame() {
        if (const auto current = ValidateContext(); current.HasError())
            return current;
        if (!rendererInitialized_) {
            return Result<void>::Failure(
                MakeGuiRendererError(RendererErrors::GuiNotInitialized, "OpenGL GUI renderer is not initialized."));
        }
        composition_.Reset();
        ImGui_ImplOpenGL3_NewFrame();
        if (nativeFontIdentity_ == 0)
            nativeFontIdentity_ = static_cast<std::uintptr_t>(ImGui::GetIO().Fonts->TexID);
        if (nativeFontIdentity_ == 0 || static_cast<std::uintptr_t>(ImGui::GetIO().Fonts->TexID) != nativeFontIdentity_)
            return Result<void>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
        ImGui_ImplSDL3_NewFrame();
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererOpenGL::PrepareComposition */
    Result<Render::GuiCompositionPreparation> EditorGuiRendererOpenGL::PrepareComposition(const Render::FrameToken frame) {
        if (const auto current = ValidateContext(); current.HasError())
            return Result<Render::GuiCompositionPreparation>::Failure(current.ErrorValue());
        return composition_.Prepare(frame, Render::GuiCompositionMode::BorrowedActiveRecording, nativeFontIdentity_,
                                    [this](const std::uintptr_t identity) -> Result<Render::GuiSampledResource> {
            if (const auto found = std::ranges::find(textures_, identity, &TextureRecord::imageIdentity); found != textures_.end())
                return Result<Render::GuiSampledResource>::Success({found->view, {}});
            const auto &viewport = ViewportBinding();
            if (viewport.textureId == identity && viewport.sampledView.IsValid())
                return Result<Render::GuiSampledResource>::Success({viewport.sampledView, viewport.sampledSampler});
            return Result<Render::GuiSampledResource>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
        });
    }

    /** @copydoc EditorGuiRendererOpenGL::RenderDrawData */
    Result<void> EditorGuiRendererOpenGL::RenderDrawData(const Render::GuiCompositionRequest &request) {
        if (const auto current = ValidateContext(); current.HasError())
            return current;
        if (!rendererInitialized_) {
            return Result<void>::Failure(
                MakeGuiRendererError(RendererErrors::GuiNotInitialized, "OpenGL GUI renderer is not initialized."));
        }
        if (!composition_.Matches(request))
            return Result<void>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
        // Backend execution restores caller bindings; GUI output belongs to the primary
        // backbuffer even when the caller retained an offscreen framebuffer binding.
        const GuiOutputScope output;
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererOpenGL::CreateTexture */
    Result<std::uintptr_t> EditorGuiRendererOpenGL::CreateTexture(const EditorRgba8ImageView &image) {
        if (const auto current = ValidateContext(); current.HasError())
            return Result<std::uintptr_t>::Failure(current.ErrorValue());
        if (!rendererInitialized_ || !image.IsValid()) {
            return Result<std::uintptr_t>::Failure(
                MakeGuiRendererError(RendererErrors::GuiInvalidTexture, "OpenGL GUI texture upload is invalid."));
        }
        auto texture = frontend_->CreateTexture(RenderMemoryScopes::GuiResources,
                                                {.extent = {image.width, image.height},
                                                 .format = Render::RenderTextureFormat::Rgba8Unorm,
                                                 .usage = Render::RenderTextureUsage::Sampled},
                                                std::as_bytes(image.pixels));
        if (texture.HasError())
            return Result<std::uintptr_t>::Failure(texture.ErrorValue());
        if (const auto processed = frontend_->ProcessResourceRequests(); processed.HasError()) {
            static_cast<void>(frontend_->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(processed.ErrorValue());
        }
        if (const auto completed = frontend_->ResourceOperationResult(texture.Value().operation); completed.HasError()) {
            static_cast<void>(frontend_->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(completed.ErrorValue());
        }
        auto view = frontend_->CreateTextureView({.texture = texture.Value().handle,
                                                  .format = Render::RenderTextureFormat::Rgba8Unorm,
                                                  .aspect = Render::RenderTextureAspect::Color});
        if (view.HasError()) {
            static_cast<void>(frontend_->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(view.ErrorValue());
        }
        const auto processed = frontend_->ProcessResourceRequests();
        if (const auto completed = frontend_->ResourceOperationResult(view.Value().operation);
            processed.HasError() || completed.HasError()) {
            const Error error = processed.HasError() ? processed.ErrorValue() : completed.ErrorValue();
            ReleaseTextureResources(texture.Value().handle, view.Value().handle);
            return Result<std::uintptr_t>::Failure(error);
        }
        auto identity = OpenGLViewportResourceBridge::EditorImageIdentity(*frontend_, view.Value().handle);
        if (identity.HasError()) {
            ReleaseTextureResources(texture.Value().handle, view.Value().handle);
            return Result<std::uintptr_t>::Failure(identity.ErrorValue());
        }
        try {
            textures_.emplace_back(identity.Value(), texture.Value().handle, view.Value().handle);
        } catch (...) {  // NOSONAR(cpp:S2738)
            ReleaseTextureResources(texture.Value().handle, view.Value().handle);
            return Result<std::uintptr_t>::Failure(
                MakeGuiRendererError(RendererErrors::GuiInvalidTexture, "OpenGL GUI texture ownership record allocation failed."));
        }
        return Result<std::uintptr_t>::Success(identity.Value());
    }

    /** @copydoc EditorGuiRendererOpenGL::ReleaseTextureResources */
    void EditorGuiRendererOpenGL::ReleaseTextureResources(const Render::RenderTextureHandle texture,
                                                          const Render::RenderTextureViewHandle view) noexcept {
        static_cast<void>(frontend_->ReleaseTextureView(view));
        static_cast<void>(frontend_->ReleaseTexture(texture));
    }

    /** @copydoc EditorGuiRendererOpenGL::DestroyTexture */
    void EditorGuiRendererOpenGL::DestroyTexture(const std::uintptr_t textureId) noexcept {
        if (const auto found = std::ranges::find(textures_, textureId, &TextureRecord::imageIdentity); found != textures_.end()) {
            ReleaseTextureResources(found->texture, found->view);
            textures_.erase(found);
        }
    }

    /** @copydoc EditorGuiRendererOpenGL::Shutdown */
    void EditorGuiRendererOpenGL::Shutdown() noexcept {
        composition_.Reset();
        BindViewportTexture({});
        nativeFontIdentity_ = 0;
        for (const TextureRecord &texture : textures_) {
            ReleaseTextureResources(texture.texture, texture.view);
        }
        textures_.clear();
        if (rendererInitialized_) {
            ImGui_ImplOpenGL3_Shutdown();
            rendererInitialized_ = false;
        }
        if (platformInitialized_) {
            ImGui_ImplSDL3_Shutdown();
            platformInitialized_ = false;
        }
    }
}  // namespace Horo::Editor
