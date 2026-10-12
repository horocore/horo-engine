#pragma once

#include "OpenGLBackendModule.h"

#include <SDL3/SDL.h>
#include <thread>

namespace Horo::Editor {
    namespace Detail {
        /** @brief The failed native operation, kept separate from an interval output value. */
        enum class SwapIntervalFailure {
            Query,
            Set,
            Mismatch
        };

        /** @brief Preserves the concrete SDL failure or exact-mode mismatch as a typed renderer result.
         * @param failure Failed operation from exact interval admission.
         * @return Typed SDL operation failure or explicit requested-mode mismatch.
         */
        [[nodiscard]] Result<void> SwapIntervalError(SwapIntervalFailure failure);

        /** @brief Admits an exact swap interval through the concrete host's native query/set operations.
         * @param interval Required native interval, not a fallback preference.
         * @param query Native readback operation; a failed query never proves an active interval.
         * @param set Native reconfiguration operation, used only when readback differs.
         * @return Success for verified exact readback; typed query, setter or mismatch failure otherwise.
         */
        template <typename Query, typename Set>
        [[nodiscard]] Result<void> ApplyExactSwapInterval(const int interval, Query query, Set set) {
            int actualInterval = 0;
            if (!query(&actualInterval))
                return SwapIntervalError(SwapIntervalFailure::Query);
            // An exact active interval needs no swap-control extension. A different
            // request must still be realized and verified; never accept a fallback.
            if (actualInterval == interval)
                return Result<void>::Success();
            if (!set(interval))
                return SwapIntervalError(SwapIntervalFailure::Set);
            if (!query(&actualInterval))
                return SwapIntervalError(SwapIntervalFailure::Query);
            return actualInterval == interval ? Result<void>::Success() : SwapIntervalError(SwapIntervalFailure::Mismatch);
        }
    }  // namespace Detail

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
