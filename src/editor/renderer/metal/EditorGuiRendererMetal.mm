#include "EditorGuiRendererMetal.h"

#include "editor/renderer/EditorRenderMemoryScopes.h"
#include "editor/renderer/metal/MetalViewportResourceBridge.h"

#import <Metal/Metal.h>
#include <algorithm>
#include <imgui.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_sdl3.h>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Error MakeGuiRendererError(const char *code, std::string message) {
            return Error{.code = ErrorCode{code},
                         .domain = ErrorDomainId{"horo.editor.gui.metal"},
                         .severity = ErrorSeverity::Error,
                         .message = std::move(message)};
        }
    }  // namespace

    struct EditorGuiRendererMetal::Impl {
        Impl(SDL_Window &borrowedWindow, Render::MetalEditorGraphicsBridge &borrowedGraphicsBridge,
             Render::RenderFrontend &borrowedFrontend) noexcept
            : window(&borrowedWindow), graphicsBridge(&borrowedGraphicsBridge), frontend(&borrowedFrontend) {}

        struct TextureRecord {
            std::uintptr_t imageIdentity{0};
            Render::RenderTextureHandle texture;
            Render::RenderTextureViewHandle view;
        };

        SDL_Window *window{nullptr};
        Render::MetalEditorGraphicsBridge *graphicsBridge{nullptr};
        Render::RenderFrontend *frontend{nullptr};
        __strong id<MTLDevice> device{nil};
        std::vector<TextureRecord> textures;
        bool platformInitialized{false};
        bool rendererInitialized{false};
    };

    /** @copydoc EditorGuiRendererMetal::EditorGuiRendererMetal */
    EditorGuiRendererMetal::EditorGuiRendererMetal(SDL_Window &window, Render::MetalEditorGraphicsBridge &graphicsBridge,
                                                   Render::RenderFrontend &frontend) noexcept
        : impl_(std::make_unique<Impl>(window, graphicsBridge, frontend)) {}

    /** @copydoc EditorGuiRendererMetal::~EditorGuiRendererMetal */
    EditorGuiRendererMetal::~EditorGuiRendererMetal() {
        Shutdown();
    }

    /** @copydoc EditorGuiRendererMetal::Initialize */
    Result<void> EditorGuiRendererMetal::Initialize() {
        if (impl_->platformInitialized || impl_->rendererInitialized) {
            return Result<void>::Failure(
                MakeGuiRendererError("editor.gui.metal.invalid_state", "Metal GUI renderer is already initialized."));
        }
        impl_->device = (__bridge id<MTLDevice>)impl_->graphicsBridge->Device();
        if (impl_->device == nil) {
            return Result<void>::Failure(
                MakeGuiRendererError("editor.gui.metal.device_unavailable", "Metal GUI renderer device is unavailable."));
        }

        impl_->platformInitialized = ImGui_ImplSDL3_InitForMetal(impl_->window);
        impl_->rendererInitialized = impl_->platformInitialized && ImGui_ImplMetal_Init(impl_->device);
        if (!impl_->rendererInitialized) {
            Shutdown();
            return Result<void>::Failure(
                MakeGuiRendererError("editor.gui.metal.initialization_failed", "Failed to initialize Dear ImGui SDL3/Metal bridges."));
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererMetal::BeginFrame */
    Result<void> EditorGuiRendererMetal::BeginFrame() {
        if (!impl_->rendererInitialized) {
            return Result<void>::Failure(
                MakeGuiRendererError("editor.gui.metal.not_initialized", "Metal GUI renderer is not initialized."));
        }
        MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)impl_->graphicsBridge->CurrentRenderPassDescriptor();
        if (pass == nil) {
            return Result<void>::Failure(
                MakeGuiRendererError("editor.gui.metal.no_active_frame", "Metal GUI frame requires an active renderer frame."));
        }
        ImGui_ImplMetal_NewFrame(pass);
        ImGui_ImplSDL3_NewFrame();
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererMetal::RenderDrawData */
    Result<void> EditorGuiRendererMetal::RenderDrawData() {
        if (!impl_->rendererInitialized) {
            return Result<void>::Failure(
                MakeGuiRendererError("editor.gui.metal.not_initialized", "Metal GUI renderer is not initialized."));
        }
        id<MTLCommandBuffer> commandBuffer = (__bridge id<MTLCommandBuffer>)impl_->graphicsBridge->CurrentCommandBuffer();
        id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)impl_->graphicsBridge->CurrentRenderEncoder();
        if (commandBuffer == nil || encoder == nil) {
            return Result<void>::Failure(
                MakeGuiRendererError("editor.gui.metal.no_primary_encoder", "Metal GUI rendering requires an active primary encoder."));
        }
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, encoder);
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererMetal::CreateTexture */
    Result<std::uintptr_t> EditorGuiRendererMetal::CreateTexture(const EditorRgba8ImageView &image) {
        if (!impl_->rendererInitialized || !image.IsValid()) {
            return Result<std::uintptr_t>::Failure(
                MakeGuiRendererError("editor.gui.metal.invalid_texture", "Metal GUI texture upload is invalid."));
        }
        auto texture = impl_->frontend->CreateTexture(RenderMemoryScopes::GuiResources,
                                                      {.extent = {image.width, image.height},
                                                       .format = Render::RenderTextureFormat::Rgba8Unorm,
                                                       .usage = Render::RenderTextureUsage::Sampled},
                                                      std::as_bytes(image.pixels));
        if (texture.HasError())
            return Result<std::uintptr_t>::Failure(texture.ErrorValue());
        if (const auto processed = impl_->frontend->ProcessResourceRequests(); processed.HasError()) {
            static_cast<void>(impl_->frontend->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(processed.ErrorValue());
        }
        if (const auto completed = impl_->frontend->ResourceOperationResult(texture.Value().operation); completed.HasError()) {
            static_cast<void>(impl_->frontend->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(completed.ErrorValue());
        }
        auto view = impl_->frontend->CreateTextureView({.texture = texture.Value().handle,
                                                        .format = Render::RenderTextureFormat::Rgba8Unorm,
                                                        .aspect = Render::RenderTextureAspect::Color});
        if (view.HasError()) {
            static_cast<void>(impl_->frontend->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(view.ErrorValue());
        }
        const auto processed = impl_->frontend->ProcessResourceRequests();
        const auto completed = impl_->frontend->ResourceOperationResult(view.Value().operation);
        if (processed.HasError() || completed.HasError()) {
            const Error error = processed.HasError() ? processed.ErrorValue() : completed.ErrorValue();
            static_cast<void>(impl_->frontend->ReleaseTextureView(view.Value().handle));
            static_cast<void>(impl_->frontend->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(error);
        }
        auto identity = MetalViewportResourceBridge::EditorImageIdentity(*impl_->frontend, view.Value().handle);
        if (identity.HasError()) {
            static_cast<void>(impl_->frontend->ReleaseTextureView(view.Value().handle));
            static_cast<void>(impl_->frontend->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(identity.ErrorValue());
        }
        try {
            impl_->textures.push_back({identity.Value(), texture.Value().handle, view.Value().handle});
        } catch (...) {  // NOSONAR(cpp:S2738)
            static_cast<void>(impl_->frontend->ReleaseTextureView(view.Value().handle));
            static_cast<void>(impl_->frontend->ReleaseTexture(texture.Value().handle));
            return Result<std::uintptr_t>::Failure(MakeGuiRendererError("editor.gui.metal.texture_record_allocation_failed",
                                                                        "Metal GUI texture ownership record allocation failed."));
        }
        return Result<std::uintptr_t>::Success(identity.Value());
    }

    /** @copydoc EditorGuiRendererMetal::DestroyTexture */
    void EditorGuiRendererMetal::DestroyTexture(const std::uintptr_t textureId) noexcept {
        const auto found = std::ranges::find(impl_->textures, textureId, &Impl::TextureRecord::imageIdentity);
        if (found != impl_->textures.end()) {
            static_cast<void>(impl_->frontend->ReleaseTextureView(found->view));
            static_cast<void>(impl_->frontend->ReleaseTexture(found->texture));
            impl_->textures.erase(found);
        }
    }

    /** @copydoc EditorGuiRendererMetal::Shutdown */
    void EditorGuiRendererMetal::Shutdown() noexcept {
        impl_->graphicsBridge->WaitUntilIdle();
        for (const Impl::TextureRecord &texture : impl_->textures) {
            static_cast<void>(impl_->frontend->ReleaseTextureView(texture.view));
            static_cast<void>(impl_->frontend->ReleaseTexture(texture.texture));
        }
        impl_->textures.clear();
        if (impl_->rendererInitialized) {
            ImGui_ImplMetal_Shutdown();
            impl_->rendererInitialized = false;
        }
        if (impl_->platformInitialized) {
            ImGui_ImplSDL3_Shutdown();
            impl_->platformInitialized = false;
        }
        impl_->device = nil;
    }
}  // namespace Horo::Editor
