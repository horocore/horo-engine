#include "OpenGLBackendInternal.h"

#include <bit>
#include <glad/gl.h>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Checks the complete fixed native command/state/sync entry-point set after dispatch loading. */
        bool ProductionIsAvailable() noexcept {
            const std::array required{
                glViewportIndexedf != nullptr, glClearColor != nullptr,      glClear != nullptr,
                glGetIntegerv != nullptr,      glGetFloatv != nullptr,       glGetBooleanv != nullptr,
                glIsEnabled != nullptr,        glEnable != nullptr,          glDisable != nullptr,
                glColorMaski != nullptr,       glBindFramebuffer != nullptr, glDrawBuffer != nullptr,
                glGetError != nullptr,         glFenceSync != nullptr,       glClientWaitSync != nullptr,
                glDeleteSync != nullptr,       glEnablei != nullptr,         glDisablei != nullptr,
                glGetFloati_v != nullptr,      glIsEnabledi != nullptr,      glFlush != nullptr,
                glDrawBuffers != nullptr,
            };
            return std::ranges::all_of(required, [](const bool available) {
                return available;
            });
        }

        void ProductionViewport(const float x, const float y, const float width, const float height) noexcept {
            glViewportIndexedf(0, x, y, width, height);
        }

        /** @brief Reports actual baseline copy dispatch readiness after context loading. */
        bool ProductionIsGraphAvailable() noexcept {
            return glCopyBufferSubData != nullptr;
        }

        /** @brief Copies checked resident ranges through the owning context's baseline dispatch. */
        void ProductionCopyBufferSubData(const std::uint32_t sourceTarget, const std::uint32_t destinationTarget,
                                         const std::size_t sourceOffset, const std::size_t destinationOffset,
                                         const std::size_t byteCount) noexcept {
            glCopyBufferSubData(sourceTarget, destinationTarget, static_cast<GLintptr>(sourceOffset),
                                static_cast<GLintptr>(destinationOffset), static_cast<GLsizeiptr>(byteCount));
        }

        void ProductionClearColor(const float red, const float green, const float blue, const float alpha) noexcept {
            glClearColor(red, green, blue, alpha);
        }

        void ProductionClear(const std::uint32_t mask) noexcept {
            glClear(mask);
        }

        void ProductionGetInteger(const std::uint32_t name, const std::span<std::int32_t> values) noexcept {
            glGetIntegerv(name, values.data());
        }

        void ProductionGetFloat(const std::uint32_t name, const std::span<float> values) noexcept {
            if (name == GL_VIEWPORT)
                glGetFloati_v(name, 0, values.data());
            else
                glGetFloatv(name, values.data());
        }

        void ProductionGetBoolean(const std::uint32_t name, const std::span<std::uint8_t> values) noexcept {
            glGetBooleanv(name, values.data());
        }

        bool ProductionIsEnabled(const std::uint32_t name) noexcept {
            return (name == GL_SCISSOR_TEST ? glIsEnabledi(name, 0) : glIsEnabled(name)) == GL_TRUE;
        }

        void ProductionSetEnabled(const std::uint32_t name, const bool enabled) noexcept {
            if (name == GL_SCISSOR_TEST) {
                if (enabled)
                    glEnablei(name, 0);
                else
                    glDisablei(name, 0);
            } else if (enabled)
                glEnable(name);
            else
                glDisable(name);
        }

        void ProductionColorMask(const std::span<const std::uint8_t, 4> mask) noexcept {
            glColorMaski(0, mask[0], mask[1], mask[2], mask[3]);
        }

        void ProductionBindDrawFramebuffer(const std::uint32_t object) noexcept {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, object);
        }

        void ProductionStateDrawBuffer(const std::uint32_t buffer) noexcept {
            glDrawBuffer(buffer);
        }

        void ProductionStateDrawBuffers(const std::span<const std::uint32_t> buffers) noexcept {
            glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
        }

        std::uint32_t ProductionError() noexcept {
            return glGetError();
        }

        std::uintptr_t ProductionFence() noexcept {
            return std::bit_cast<std::uintptr_t>(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
        }

        std::uint32_t ProductionPoll(const std::uintptr_t fence) noexcept {
            // Zero timeout and no flush flag: normal frames never wait for GPU completion.
            return glClientWaitSync(std::bit_cast<GLsync>(fence), 0, 0);
        }

        void ProductionDestroyFence(const std::uintptr_t fence) noexcept {
            glDeleteSync(std::bit_cast<GLsync>(fence));
        }

        void ProductionFlush() noexcept {
            glFlush();
        }

        void ProductionGenerateBuffers(const std::int32_t count, std::uint32_t *objects) {
            glGenBuffers(count, objects);
        }

        void ProductionDeleteBuffers(const std::int32_t count, const std::uint32_t *objects) {
            glDeleteBuffers(count, objects);
        }

        void ProductionBindBuffer(const std::uint32_t target, const std::uint32_t object) {
            glBindBuffer(target, object);
        }

        void ProductionBufferData(const std::uint32_t target, const std::size_t byteSize, const std::span<const std::byte> data,
                                  const std::uint32_t usage) {
            glBufferData(target, static_cast<GLsizeiptr>(byteSize), data.empty() ? nullptr : data.data(), usage);
        }

        void ProductionGenerateVertexArrays(const std::int32_t count, std::uint32_t *objects) {
            glGenVertexArrays(count, objects);
        }

        void ProductionDeleteVertexArrays(const std::int32_t count, const std::uint32_t *objects) {
            glDeleteVertexArrays(count, objects);
        }

        void ProductionBindVertexArray(const std::uint32_t, const std::uint32_t object) {
            glBindVertexArray(object);
        }

        void ProductionVertexAttributePointer(const std::uint32_t index, const std::int32_t size, const std::uint32_t type,
                                              const std::uint8_t normalized, const std::int32_t stride, const std::uintptr_t offset) {
            glVertexAttribPointer(index, size, type, normalized, stride,
                                  std::bit_cast<const void *>(offset));  // NOSONAR: OpenGL models byte offsets as pointers.
        }

        void ProductionEnableVertexAttribute(const std::uint32_t index) {
            glEnableVertexAttribArray(index);
        }

        void ProductionGenerateTextures(const std::int32_t count, std::uint32_t *objects) {
            glGenTextures(count, objects);
        }

        void ProductionDeleteTextures(const std::int32_t count, const std::uint32_t *objects) {
            glDeleteTextures(count, objects);
        }

        void ProductionBindTexture(const std::uint32_t target, const std::uint32_t object) {
            glBindTexture(target, object);
        }

        void ProductionTextureParameter(const std::uint32_t target, const std::uint32_t parameter, const std::int32_t value) {
            glTexParameteri(target, parameter, value);
        }

        void ProductionTextureImage(const OpenGLTextureImageDescriptor &descriptor) {
            glTexImage2D(descriptor.target, descriptor.level, descriptor.internalFormat, descriptor.width, descriptor.height,
                         descriptor.border, descriptor.format, descriptor.type,
                         descriptor.initialData.empty() ? nullptr : descriptor.initialData.data());
        }

        void ProductionGenerateFramebuffers(const std::int32_t count, std::uint32_t *objects) {
            glGenFramebuffers(count, objects);
        }

        void ProductionDeleteFramebuffers(const std::int32_t count, const std::uint32_t *objects) {
            glDeleteFramebuffers(count, objects);
        }

        void ProductionBindFramebuffer(const std::uint32_t target, const std::uint32_t object) {
            glBindFramebuffer(target, object);
        }

        void ProductionFramebufferTexture(const std::uint32_t target, const std::uint32_t attachment, const std::uint32_t texture,
                                          const std::int32_t level) {
            glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, texture, level);
        }

        std::uint32_t ProductionCheckFramebuffer(const std::uint32_t target) {
            return glCheckFramebufferStatus(target);
        }

        void ProductionDrawBuffer(const std::uint32_t mode) {
            glDrawBuffer(mode);
        }

        void ProductionReadBuffer(const std::uint32_t mode) {
            glReadBuffer(mode);
        }
    }  // namespace

    OpenGLCommandFunctions ProductionOpenGLCommandFunctions() noexcept {
        return OpenGLCommandFunctions{
            .isAvailable = &ProductionIsAvailable,
            .isGraphAvailable = &ProductionIsGraphAvailable,
            .viewport = &ProductionViewport,
            .clearColor = &ProductionClearColor,
            .clear = &ProductionClear,
            .state = {.getInteger = &ProductionGetInteger,
                      .getFloat = &ProductionGetFloat,
                      .getBoolean = &ProductionGetBoolean,
                      .isEnabled = &ProductionIsEnabled,
                      .setEnabled = &ProductionSetEnabled,
                      .colorMask = &ProductionColorMask,
                      .bindDrawFramebuffer = &ProductionBindDrawFramebuffer,
                      .drawBuffer = &ProductionStateDrawBuffer,
                      .drawBuffers = &ProductionStateDrawBuffers,
                      .error = &ProductionError},
            .sync = {.fence = &ProductionFence, .poll = &ProductionPoll, .destroy = &ProductionDestroyFence, .flush = &ProductionFlush},
            .buffers = {.generateBuffers = &ProductionGenerateBuffers,
                        .deleteBuffers = &ProductionDeleteBuffers,
                        .bindBuffer = &ProductionBindBuffer,
                        .bufferData = &ProductionBufferData,
                        .copyBufferSubData = &ProductionCopyBufferSubData},
            .vertexArrays = {.generateVertexArrays = &ProductionGenerateVertexArrays,
                             .deleteVertexArrays = &ProductionDeleteVertexArrays,
                             .bindVertexArray = &ProductionBindVertexArray,
                             .vertexAttributePointer = &ProductionVertexAttributePointer,
                             .enableVertexAttribute = &ProductionEnableVertexAttribute},
            .textures = {.generateTextures = &ProductionGenerateTextures,
                         .deleteTextures = &ProductionDeleteTextures,
                         .bindTexture = &ProductionBindTexture,
                         .textureParameter = &ProductionTextureParameter,
                         .textureImage = &ProductionTextureImage},
            .framebuffers = {.generateFramebuffers = &ProductionGenerateFramebuffers,
                             .deleteFramebuffers = &ProductionDeleteFramebuffers,
                             .bindFramebuffer = &ProductionBindFramebuffer,
                             .framebufferTexture = &ProductionFramebufferTexture,
                             .checkFramebuffer = &ProductionCheckFramebuffer,
                             .drawBuffer = &ProductionDrawBuffer,
                             .readBuffer = &ProductionReadBuffer},
        };
    }
}  // namespace Horo::Render::Detail
