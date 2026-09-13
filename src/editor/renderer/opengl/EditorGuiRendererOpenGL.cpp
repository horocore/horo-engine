#include "EditorGuiRendererOpenGL.h"

#include "editor/renderer/EditorRenderMemoryScopes.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/renderer/opengl/OpenGLViewportResourceBridge.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <SDL3/SDL_opengl.h>
#endif

#include <algorithm>
#include <cstddef>
#include <string>

namespace Horo::Editor {
    namespace {
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

    /** @copydoc EditorGuiRendererOpenGL::Initialize */
    Result<void> EditorGuiRendererOpenGL::Initialize() {
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
        if (!rendererInitialized_) {
            return Result<void>::Failure(
                MakeGuiRendererError(RendererErrors::GuiNotInitialized, "OpenGL GUI renderer is not initialized."));
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererOpenGL::RenderDrawData */
    Result<void> EditorGuiRendererOpenGL::RenderDrawData() {
        if (!rendererInitialized_) {
            return Result<void>::Failure(
                MakeGuiRendererError(RendererErrors::GuiNotInitialized, "OpenGL GUI renderer is not initialized."));
        }
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererOpenGL::CreateTexture */
    Result<std::uintptr_t> EditorGuiRendererOpenGL::CreateTexture(const EditorRgba8ImageView &image) {
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
            static_cast<void>(frontend_->ReleaseTextureView(view.Value().handle));
            static_cast<void>(frontend_->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(error);
        }
        auto identity = OpenGLViewportResourceBridge::EditorImageIdentity(*frontend_, view.Value().handle);
        if (identity.HasError()) {
            static_cast<void>(frontend_->ReleaseTextureView(view.Value().handle));
            static_cast<void>(frontend_->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(identity.ErrorValue());
        }
        try {
            textures_.emplace_back(identity.Value(), texture.Value().handle, view.Value().handle);
        } catch (...) {  // NOSONAR(cpp:S2738)
            static_cast<void>(frontend_->ReleaseTextureView(view.Value().handle));
            static_cast<void>(frontend_->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(
                MakeGuiRendererError(RendererErrors::GuiInvalidTexture, "OpenGL GUI texture ownership record allocation failed."));
        }
        return Result<std::uintptr_t>::Success(identity.Value());
    }

    /** @copydoc EditorGuiRendererOpenGL::DestroyTexture */
    void EditorGuiRendererOpenGL::DestroyTexture(const std::uintptr_t textureId) noexcept {
        if (const auto found = std::ranges::find(textures_, textureId, &TextureRecord::imageIdentity); found != textures_.end()) {
            static_cast<void>(frontend_->ReleaseTextureView(found->view));
            static_cast<void>(frontend_->ReleaseTexture(found->texture));
            textures_.erase(found);
        }
    }

    /** @copydoc EditorGuiRendererOpenGL::Shutdown */
    void EditorGuiRendererOpenGL::Shutdown() noexcept {
        for (const TextureRecord &texture : textures_) {
            static_cast<void>(frontend_->ReleaseTextureView(texture.view));
            static_cast<void>(frontend_->ReleaseTexture(texture.texture));
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
