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
        rasterizerDiscard_ = state.isEnabled(GL_RASTERIZER_DISCARD);
    }

    /** @copydoc OpenGLExecutionState::Apply */
    bool OpenGLExecutionState::Apply(const FramebufferExtent extent) noexcept {
        const auto &state = functions_.state;
        std::int32_t count{};
        state.getInteger(GL_MAX_DRAW_BUFFERS, std::span{&count, 1});
        if (count <= 0 || static_cast<std::size_t>(count) > defaultDrawBuffers_.size())
            return false;
        drawBufferCount_ = static_cast<std::size_t>(count);
        state.bindDrawFramebuffer(0);
        for (std::size_t index = 0; index < drawBufferCount_; ++index) {
            std::int32_t buffer{};
            state.getInteger(GL_DRAW_BUFFER0 + static_cast<std::uint32_t>(index), std::span{&buffer, 1});
            defaultDrawBuffers_[index] = static_cast<std::uint32_t>(buffer);
        }
        applied_ = true;
        state.drawBuffer(GL_BACK);
        state.setEnabled(GL_SCISSOR_TEST, false);
        state.setEnabled(GL_DITHER, false);
        state.setEnabled(GL_FRAMEBUFFER_SRGB, false);
        state.setEnabled(GL_RASTERIZER_DISCARD, false);
        constexpr std::array<std::uint8_t, 4> unmasked{1, 1, 1, 1};
        state.colorMask(unmasked);
        functions_.viewport(0, 0, static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height));
        return true;
    }

    /** @copydoc OpenGLExecutionState::~OpenGLExecutionState */
    OpenGLExecutionState::~OpenGLExecutionState() noexcept {
        if (!applied_)
            return;
        const auto &state = functions_.state;
        state.bindDrawFramebuffer(0);
        const std::uint32_t first = defaultDrawBuffers_[0];
        // Aggregate selectors are legal only for DrawBuffer; that API guarantees higher slots are NONE.
        if (first == GL_FRONT || first == GL_BACK || first == GL_LEFT || first == GL_RIGHT || first == GL_FRONT_AND_BACK)
            state.drawBuffer(first);
        else
            state.drawBuffers(std::span{defaultDrawBuffers_}.first(drawBufferCount_));
        state.bindDrawFramebuffer(static_cast<std::uint32_t>(framebuffer_));
        functions_.viewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        functions_.clearColor(clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]);
        state.colorMask(colorMask_);
        state.setEnabled(GL_SCISSOR_TEST, scissor_);
        state.setEnabled(GL_DITHER, dither_);
        state.setEnabled(GL_FRAMEBUFFER_SRGB, srgb_);
        state.setEnabled(GL_RASTERIZER_DISCARD, rasterizerDiscard_);
    }
}  // namespace Horo::Render::Detail
