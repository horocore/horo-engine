#include "OpenGLStateSnapshot.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        void RestoreCapability(const GLenum capability, const GLboolean enabled) noexcept {
            if (enabled == GL_TRUE)
                glEnable(capability);
            else
                glDisable(capability);
        }
    }  // namespace

    /** @copydoc OpenGLStateSnapshot::OpenGLStateSnapshot */
    OpenGLStateSnapshot::OpenGLStateSnapshot(const std::uint32_t shadowUnit, const std::span<const std::uint32_t> uniformSlots) noexcept
        : shadowUnit_(shadowUnit) {
        depthTestEnabled_ = glIsEnabled(GL_DEPTH_TEST);
        scissorTestEnabled_ = glIsEnabled(GL_SCISSOR_TEST);
        blendEnabled_ = glIsEnabled(GL_BLEND);
        cullFaceEnabled_ = glIsEnabled(GL_CULL_FACE);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer_);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray_);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer_);
        glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &uniformBuffer_);
        uniformCount_ = std::min(uniformSlots.size(), uniforms_.size());
        for (std::size_t index = 0; index < uniformCount_; ++index) {
            auto &binding = uniforms_[index];
            binding.slot = uniformSlots[index];
            glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, binding.slot, &binding.buffer);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_START, binding.slot, &binding.offset);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, binding.slot, &binding.size);
        }
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunction_);
        glGetIntegerv(GL_CULL_FACE_MODE, &cullFaceMode_);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
        glActiveTexture(GL_TEXTURE0 + shadowUnit_);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &shadowTexture_);
        glGetIntegerv(GL_VIEWPORT, viewport_.data());
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor_.data());
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask_.data());
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask_);
    }

    /** @copydoc OpenGLStateSnapshot::~OpenGLStateSnapshot */
    OpenGLStateSnapshot::~OpenGLStateSnapshot() {
        glBindVertexArray(static_cast<GLuint>(vertexArray_));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer_));
        for (std::size_t index = 0; index < uniformCount_; ++index) {
            const auto &binding = uniforms_[index];
            if (binding.buffer != 0 && binding.size > 0)
                glBindBufferRange(GL_UNIFORM_BUFFER, binding.slot, static_cast<GLuint>(binding.buffer),
                                  static_cast<GLintptr>(binding.offset), static_cast<GLsizeiptr>(binding.size));
            else
                glBindBufferBase(GL_UNIFORM_BUFFER, binding.slot, static_cast<GLuint>(binding.buffer));
        }
        glBindBuffer(GL_UNIFORM_BUFFER, static_cast<GLuint>(uniformBuffer_));
        glUseProgram(static_cast<GLuint>(program_));
        glDepthFunc(static_cast<GLenum>(depthFunction_));
        RestoreCapability(GL_DEPTH_TEST, depthTestEnabled_);
        RestoreCapability(GL_SCISSOR_TEST, scissorTestEnabled_);
        RestoreCapability(GL_BLEND, blendEnabled_);
        RestoreCapability(GL_CULL_FACE, cullFaceEnabled_);
        glCullFace(static_cast<GLenum>(cullFaceMode_));
        glColorMask(colorMask_[0], colorMask_[1], colorMask_[2], colorMask_[3]);
        glDepthMask(depthMask_);
        glClearColor(clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer_));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer_));
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glActiveTexture(GL_TEXTURE0 + shadowUnit_);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(shadowTexture_));
        glActiveTexture(static_cast<GLenum>(activeTexture_));
    }
}  // namespace Horo::Editor
