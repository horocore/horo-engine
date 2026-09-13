#pragma once

#include "runtime/renderer/modules/opengl/OpenGLBackendModule.h"

#include <SDL3/SDL.h>

namespace Horo::Editor {
    /** @brief SDL3 implementation of the OpenGL module's native-free presentation port. */
    class SdlOpenGLPresentationPort final : public Render::IOpenGLPresentationPort {
    public:
        /** @brief Borrows the platform window; the window must outlive this port and renderer frontend. */
        explicit SdlOpenGLPresentationPort(SDL_Window &window) noexcept;

        [[nodiscard]] Result<void> CreateContext(const Render::OpenGLContextDescriptor &descriptor) override;
        [[nodiscard]] Result<void> MakeCurrent() override;
        [[nodiscard]] Result<void> LoadCommandDispatch() override;
        [[nodiscard]] Result<Render::OpenGLContextFacts> QueryContextFacts() override;
        [[nodiscard]] Result<void> SetPresentMode(Render::PresentMode mode) override;
        [[nodiscard]] Result<void> SwapBuffers() override;
        void DestroyContext() noexcept override;

        /** @brief Returns the retained context for the editor-private ImGui OpenGL bridge. */
        [[nodiscard]] SDL_GLContext Context() const noexcept;

    private:
        SDL_Window *window_{nullptr};
        SDL_GLContext context_{nullptr};
    };
}  // namespace Horo::Editor
