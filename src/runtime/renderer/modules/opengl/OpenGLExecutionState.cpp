#include "OpenGLExecutionState.h"

#include <glad/gl.h>

namespace Horo::Render::Detail {
    /** @copydoc OpenGLExecutionState::OpenGLExecutionState */
    OpenGLExecutionState::OpenGLExecutionState(const OpenGLCommandFunctions &functions) noexcept : functions_(functions) {
        const auto &state = functions_.state;
        state.getInteger(GL_VIEWPORT, viewport_);
        state.getInteger(GL_DRAW_FRAMEBUFFER_BINDING, std::span{&framebuffer_, 1});
        state.getFloat(GL_COLOR_CLEAR_VALUE, clearColor_);
        state.getBoolean(GL_COLOR_WRITEMASK, colorMask_);
        scissor_ = state.isEnabled(GL_SCISSOR_TEST);
        dither_ = state.isEnabled(GL_DITHER);
        srgb_ = state.isEnabled(GL_FRAMEBUFFER_SRGB);
    }

    /** @copydoc OpenGLExecutionState::Apply */
    void OpenGLExecutionState::Apply(const FramebufferExtent extent) noexcept {
        const auto &state = functions_.state;
        state.bindDrawFramebuffer(0);
        state.getInteger(GL_DRAW_BUFFER, std::span{&defaultDrawBuffer_, 1});
        applied_ = true;
        state.drawBuffer(GL_BACK);
        state.setEnabled(GL_SCISSOR_TEST, false);
        state.setEnabled(GL_DITHER, false);
        state.setEnabled(GL_FRAMEBUFFER_SRGB, false);
        constexpr std::array<std::uint8_t, 4> unmasked{1, 1, 1, 1};
        state.colorMask(unmasked);
        functions_.viewport(0, 0, static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height));
    }

    /** @copydoc OpenGLExecutionState::~OpenGLExecutionState */
    OpenGLExecutionState::~OpenGLExecutionState() noexcept {
        if (!applied_)
            return;
        const auto &state = functions_.state;
        state.bindDrawFramebuffer(0);
        state.drawBuffer(static_cast<std::uint32_t>(defaultDrawBuffer_));
        state.bindDrawFramebuffer(static_cast<std::uint32_t>(framebuffer_));
        functions_.viewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        functions_.clearColor(clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]);
        state.colorMask(colorMask_);
        state.setEnabled(GL_SCISSOR_TEST, scissor_);
        state.setEnabled(GL_DITHER, dither_);
        state.setEnabled(GL_FRAMEBUFFER_SRGB, srgb_);
    }
}  // namespace Horo::Render::Detail
