#pragma once

/** @file OpenGLExecutionState.h
 * @brief Private allocation-free state isolation for OpenGL primary-output encoding.
 */

#include "OpenGLBackendInternal.h"

namespace Horo::Render::Detail {
    /** @brief Allocation-free scoped isolation of every state changed by primary-output command adaptation.
     * Native state callbacks are non-throwing. Destruction restores the default framebuffer's draw
     * buffer before rebinding the caller's framebuffer, whose separate draw-buffer state is untouched.
     */
    class OpenGLExecutionState final {
    public:
        /** @brief Captures caller state without changing the current framebuffer.
         * @param functions Borrowed non-throwing state dispatch, which must outlive the guard.
         */
        explicit OpenGLExecutionState(const OpenGLCommandFunctions &functions) noexcept;
        /** @brief Restores captured state after Apply, including during command exception unwinding. */
        ~OpenGLExecutionState() noexcept;
        OpenGLExecutionState(const OpenGLExecutionState &) = delete;
        OpenGLExecutionState &operator=(const OpenGLExecutionState &) = delete;

        /** @brief Selects deterministic, unmasked primary backbuffer commands for the exact active extent.
         * @param extent Validated active-frame extent representable by signed native dimensions.
         * @pre Called at most once on a current double-buffered presentation context.
         */
        void Apply(FramebufferExtent extent) noexcept;

    private:
        const OpenGLCommandFunctions &functions_;
        std::array<std::int32_t, 4> viewport_{};
        std::array<float, 4> clearColor_{};
        std::array<std::uint8_t, 4> colorMask_{};
        std::int32_t framebuffer_{};
        std::int32_t defaultDrawBuffer_{};
        bool scissor_{};
        bool dither_{};
        bool srgb_{};
        bool applied_{};
    };
}  // namespace Horo::Render::Detail
