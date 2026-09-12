#pragma once

#include "OpenGLBackendModule.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace Horo::Render::Detail {
    using OpenGLViewportFunction = void (*)(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
    using OpenGLClearColorFunction = void (*)(float red, float green, float blue, float alpha);
    using OpenGLClearFunction = void (*)(std::uint32_t mask);
    using OpenGLGenerateObjectsFunction = void (*)(std::int32_t count, std::uint32_t *objects);
    using OpenGLDeleteObjectsFunction = void (*)(std::int32_t count, const std::uint32_t *objects);
    using OpenGLBindObjectFunction = void (*)(std::uint32_t target, std::uint32_t object);
    using OpenGLBufferDataFunction = void (*)(std::uint32_t target, std::size_t byteSize, std::span<const std::byte> data,
                                              std::uint32_t usage);
    using OpenGLVertexAttributePointerFunction = void (*)(std::uint32_t index, std::int32_t size, std::uint32_t type,
                                                          std::uint8_t normalized, std::int32_t stride, std::uintptr_t offset);
    using OpenGLEnableVertexAttributeFunction = void (*)(std::uint32_t index);
    using OpenGLTextureParameterFunction = void (*)(std::uint32_t target, std::uint32_t parameter, std::int32_t value);

    struct OpenGLTextureImageDescriptor {
        std::uint32_t target{0};
        std::int32_t level{0};
        std::int32_t internalFormat{0};
        std::int32_t width{0};
        std::int32_t height{0};
        std::int32_t border{0};
        std::uint32_t format{0};
        std::uint32_t type{0};
        std::span<const std::byte> initialData;
    };

    using OpenGLTextureImageFunction = void (*)(const OpenGLTextureImageDescriptor &descriptor);
    using OpenGLFramebufferTextureFunction = void (*)(std::uint32_t target, std::uint32_t attachment, std::uint32_t texture,
                                                      std::int32_t level);
    using OpenGLCheckFramebufferFunction = std::uint32_t (*)(std::uint32_t target);
    using OpenGLDrawReadBufferFunction = void (*)(std::uint32_t mode);

    struct OpenGLBufferFunctions {
        OpenGLGenerateObjectsFunction generateBuffers{nullptr};
        OpenGLDeleteObjectsFunction deleteBuffers{nullptr};
        OpenGLBindObjectFunction bindBuffer{nullptr};
        OpenGLBufferDataFunction bufferData{nullptr};
    };

    struct OpenGLVertexArrayFunctions {
        OpenGLGenerateObjectsFunction generateVertexArrays{nullptr};
        OpenGLDeleteObjectsFunction deleteVertexArrays{nullptr};
        OpenGLBindObjectFunction bindVertexArray{nullptr};
        OpenGLVertexAttributePointerFunction vertexAttributePointer{nullptr};
        OpenGLEnableVertexAttributeFunction enableVertexAttribute{nullptr};
    };

    struct OpenGLTextureFunctions {
        OpenGLGenerateObjectsFunction generateTextures{nullptr};
        OpenGLDeleteObjectsFunction deleteTextures{nullptr};
        OpenGLBindObjectFunction bindTexture{nullptr};
        OpenGLTextureParameterFunction textureParameter{nullptr};
        OpenGLTextureImageFunction textureImage{nullptr};
    };

    struct OpenGLFramebufferFunctions {
        OpenGLGenerateObjectsFunction generateFramebuffers{nullptr};
        OpenGLDeleteObjectsFunction deleteFramebuffers{nullptr};
        OpenGLBindObjectFunction bindFramebuffer{nullptr};
        OpenGLFramebufferTextureFunction framebufferTexture{nullptr};
        OpenGLCheckFramebufferFunction checkFramebuffer{nullptr};
        OpenGLDrawReadBufferFunction drawBuffer{nullptr};
        OpenGLDrawReadBufferFunction readBuffer{nullptr};
    };

    struct OpenGLCommandFunctions {
        OpenGLViewportFunction viewport{nullptr};
        OpenGLClearColorFunction clearColor{nullptr};
        OpenGLClearFunction clear{nullptr};
        OpenGLBufferFunctions buffers;
        OpenGLVertexArrayFunctions vertexArrays;
        OpenGLTextureFunctions textures;
        OpenGLFramebufferFunctions framebuffers;

        [[nodiscard]] bool IsValid() const noexcept {
            return viewport != nullptr && clearColor != nullptr && clear != nullptr;
        }

        [[nodiscard]] bool HasResourceFunctions() const noexcept {
            const std::array available{
                buffers.generateBuffers != nullptr,
                buffers.deleteBuffers != nullptr,
                buffers.bindBuffer != nullptr,
                buffers.bufferData != nullptr,
                vertexArrays.generateVertexArrays != nullptr,
                vertexArrays.deleteVertexArrays != nullptr,
                vertexArrays.bindVertexArray != nullptr,
                vertexArrays.vertexAttributePointer != nullptr,
                vertexArrays.enableVertexAttribute != nullptr,
                textures.generateTextures != nullptr,
                textures.deleteTextures != nullptr,
                textures.bindTexture != nullptr,
                textures.textureParameter != nullptr,
                textures.textureImage != nullptr,
                framebuffers.generateFramebuffers != nullptr,
                framebuffers.deleteFramebuffers != nullptr,
                framebuffers.bindFramebuffer != nullptr,
                framebuffers.framebufferTexture != nullptr,
                framebuffers.checkFramebuffer != nullptr,
                framebuffers.drawBuffer != nullptr,
                framebuffers.readBuffer != nullptr,
            };
            return std::ranges::all_of(available, std::identity{});
        }
    };

    [[nodiscard]] OpenGLCommandFunctions ProductionOpenGLCommandFunctions() noexcept;
    [[nodiscard]] Result<void> RegisterOpenGLRenderBackendWithFunctions(RenderBackendRegistry &registry,
                                                                        IOpenGLPresentationPort &presentationPort,
                                                                        OpenGLBackendOptions options,
                                                                        const OpenGLCommandFunctions &functions);
}  // namespace Horo::Render::Detail
