#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "editor/renderer/EditorGuiComposition.h"
#include "editor/renderer/EditorGuiRenderer.h"

#include <SDL3/SDL.h>
#include <thread>
#include <vector>

namespace Horo::Editor {
    /** @brief Dear ImGui SDL3/OpenGL implementation for the editor composition. */
    class EditorGuiRendererOpenGL final : public IEditorGuiRenderer {
    public:
        /** @brief Borrows the SDL window and initialized OpenGL context. */
        EditorGuiRendererOpenGL(SDL_Window &window, SDL_GLContext context, Render::RenderFrontend &frontend) noexcept;
        ~EditorGuiRendererOpenGL() override;
        EditorGuiRendererOpenGL(const EditorGuiRendererOpenGL &) = delete;
        EditorGuiRendererOpenGL &operator=(const EditorGuiRendererOpenGL &) = delete;
        EditorGuiRendererOpenGL(EditorGuiRendererOpenGL &&) = delete;
        EditorGuiRendererOpenGL &operator=(EditorGuiRendererOpenGL &&) = delete;

        [[nodiscard]] Result<void> Initialize() override;
        [[nodiscard]] Result<void> BeginFrame() override;
        [[nodiscard]] Result<Render::GuiCompositionPreparation> PrepareComposition(Render::FrameToken frame) override;
        [[nodiscard]] Result<void> RenderDrawData(const Render::GuiCompositionRequest &request) override;
        [[nodiscard]] Result<std::uintptr_t> CreateTexture(const EditorRgba8ImageView &image) override;
        void DestroyTexture(std::uintptr_t textureId) noexcept override;
        void Shutdown() noexcept override;

    private:
        /** @brief Restores only this owner's existing window/context before CPU GUI device work.
         * @return Success or a typed owner/context activation failure; never admits a native render frame.
         */
        [[nodiscard]] Result<void> MakeContextCurrent() const;
        /** @brief Rejects a different thread or a context/window that is no longer current. */
        [[nodiscard]] Result<void> ValidateContext() const;
        /** @brief Releases a view before its backing texture without losing cleanup on upload failure. */
        void ReleaseTextureResources(Render::RenderTextureHandle texture, Render::RenderTextureViewHandle view) noexcept;
        const std::thread::id ownerThread_{std::this_thread::get_id()};
        SDL_Window *window_{nullptr};
        SDL_GLContext context_{nullptr};
        Render::RenderFrontend *frontend_{nullptr};

        struct TextureRecord {
            std::uintptr_t imageIdentity{0};
            Render::RenderTextureHandle texture;
            Render::RenderTextureViewHandle view;
        };

        std::vector<TextureRecord> textures_;
        EditorGuiComposition composition_;
        std::uintptr_t nativeFontIdentity_{};
        bool platformInitialized_{false};
        bool rendererInitialized_{false};
    };
}  // namespace Horo::Editor
