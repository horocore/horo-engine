#pragma once

#include "OpenGLBackendModule.h"

#include <SDL3/SDL.h>
#include <thread>

namespace Horo::Editor {
    /** @brief SDL3 implementation of the OpenGL module's native-free presentation port. */
    class SdlOpenGLPresentationPort final : public Render::IOpenGLPresentationPort {
    public:
        /** @brief Borrows the platform window on the render owner thread; it must outlive this port and frontend. */
        explicit SdlOpenGLPresentationPort(SDL_Window &window) noexcept;

        [[nodiscard]] Result<void> CreateContext(const Render::OpenGLContextDescriptor &descriptor) override;
        [[nodiscard]] Result<void> MakeCurrent() override;
        [[nodiscard]] Result<void> LoadCommandDispatch() override;
        [[nodiscard]] Result<Render::OpenGLContextFacts> QueryContextFacts() override;
        /** @brief Applies an exact mode to the retained current context and verifies the native swap interval.
         * @param mode Explicit FIFO or Immediate request; other modes are rejected without fallback.
         * @return Success only for a confirmed exact native interval; typed setup/query/mismatch failure otherwise.
         */
        [[nodiscard]] Result<void> SetPresentMode(Render::PresentMode mode) override;
        [[nodiscard]] Result<void> SwapBuffers() override;
        void DestroyContext() noexcept override;

        /** @brief Returns the retained context for the editor-private ImGui OpenGL bridge. */
        [[nodiscard]] SDL_GLContext Context() const noexcept;

    private:
        /** @brief Rejects a foreign caller before reading context state or invoking SDL. */
        [[nodiscard]] Result<void> CheckOwner() const;
        const std::thread::id ownerThread_;
        SDL_Window *window_{nullptr};
        SDL_GLContext context_{nullptr};
    };
}  // namespace Horo::Editor
