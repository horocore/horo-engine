#include "OpenGLBackendInternal.h"

#include <bit>
#include <glad/gl.h>

namespace Horo::Render::Detail {
    namespace {
        void ProductionViewport(const std::int32_t x, const std::int32_t y, const std::int32_t width, const std::int32_t height) {
            glViewport(x, y, width, height);
        }

        void ProductionClearColor(const float red, const float green, const float blue, const float alpha) {
            glClearColor(red, green, blue, alpha);
        }

        void ProductionClear(const std::uint32_t mask) {
            glClear(mask);
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
            .viewport = &ProductionViewport,
            .clearColor = &ProductionClearColor,
            .clear = &ProductionClear,
            .buffers = {.generateBuffers = &ProductionGenerateBuffers,
                        .deleteBuffers = &ProductionDeleteBuffers,
                        .bindBuffer = &ProductionBindBuffer,
                        .bufferData = &ProductionBufferData},
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
