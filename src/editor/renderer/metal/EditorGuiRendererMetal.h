#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "editor/renderer/EditorGuiRenderer.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"

#include <SDL3/SDL.h>
#include <memory>

namespace Horo::Editor {
    /** @brief Dear ImGui SDL3/Metal implementation for the editor composition. */
    class EditorGuiRendererMetal final : public IEditorGuiRenderer {
    public:
        /** @brief Borrows the SDL window and initialized runtime Metal bridge. */
        EditorGuiRendererMetal(SDL_Window &window, Render::MetalEditorGraphicsBridge &graphicsBridge,
                               Render::RenderFrontend &frontend) noexcept;
        ~EditorGuiRendererMetal() override;

        [[nodiscard]] Result<void> Initialize() override;
        [[nodiscard]] Result<void> BeginFrame() override;
        [[nodiscard]] Result<void> RenderDrawData() override;
        [[nodiscard]] Result<std::uintptr_t> CreateTexture(const EditorRgba8ImageView &image) override;
        void DestroyTexture(std::uintptr_t textureId) noexcept override;
        void Shutdown() noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Editor
