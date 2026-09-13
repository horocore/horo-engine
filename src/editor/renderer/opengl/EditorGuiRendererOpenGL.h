#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "editor/renderer/EditorGuiRenderer.h"

#include <SDL3/SDL.h>
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
        [[nodiscard]] Result<void> RenderDrawData() override;
        [[nodiscard]] Result<std::uintptr_t> CreateTexture(const EditorRgba8ImageView &image) override;
        void DestroyTexture(std::uintptr_t textureId) noexcept override;
        void Shutdown() noexcept override;

    private:
        SDL_Window *window_{nullptr};
        SDL_GLContext context_{nullptr};
        Render::RenderFrontend *frontend_{nullptr};

        struct TextureRecord {
            std::uintptr_t imageIdentity{0};
            Render::RenderTextureHandle texture;
            Render::RenderTextureViewHandle view;
        };

        std::vector<TextureRecord> textures_;
        bool platformInitialized_{false};
        bool rendererInitialized_{false};
    };
}  // namespace Horo::Editor
