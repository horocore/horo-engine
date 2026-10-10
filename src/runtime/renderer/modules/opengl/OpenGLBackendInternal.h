#pragma once

#include "OpenGLBackendModule.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace Horo::Render::Detail {
    /** @brief Admits actual desktop/Core context facts before backend readiness publication.
     * @param facts Realized native context version, profile, entry-point and baseline limit facts.
     * @param options Explicit host-requested OpenGL version policy.
     * @return Typed unsupported-context/driver failure or success.
     */
    [[nodiscard]] Result<void> ValidateOpenGLContextFacts(const OpenGLContextFacts &facts, const OpenGLBackendOptions &options);

    /** @brief Projects realized baseline facts into the backend-neutral capability snapshot.
     * @param facts Validated context facts supplied by the current presentation port.
     * @param resourcesAvailable Whether the complete optional resource command dispatch is available.
     * @return Explicit capabilities of this OpenGL backend incarnation.
     */
    [[nodiscard]] RenderCapabilitySnapshot MakeOpenGLCapabilitySnapshot(const OpenGLContextFacts &facts, bool resourcesAvailable) noexcept;

    /** Sets viewport zero only, preserving fractional bounds and higher indexed viewports. */
    using OpenGLViewportFunction = void (*)(float x, float y, float width, float height) noexcept;
    using OpenGLClearColorFunction = void (*)(float red, float green, float blue, float alpha) noexcept;
    using OpenGLClearFunction = void (*)(std::uint32_t mask) noexcept;
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

    /** @brief Core state operations used only while the backend's context is current; callbacks must not throw. */
    struct OpenGLStateFunctions {
        void (*getInteger)(std::uint32_t, std::span<std::int32_t>) noexcept {nullptr};
        /** VIEWPORT is queried at index zero without truncating fractional values. */
        void (*getFloat)(std::uint32_t, std::span<float>) noexcept {nullptr};
        void (*getBoolean)(std::uint32_t, std::span<std::uint8_t>) noexcept {nullptr};
        /** SCISSOR_TEST is queried only at index zero. */
        bool (*isEnabled)(std::uint32_t) noexcept {nullptr};
        /** SCISSOR_TEST changes only index zero; other capabilities are non-indexed. */
        void (*setEnabled)(std::uint32_t, bool) noexcept {nullptr};
        /** Changes only indexed color mask zero; higher masks remain untouched. */
        void (*colorMask)(std::span<const std::uint8_t, 4>) noexcept {nullptr};
        void (*bindDrawFramebuffer)(std::uint32_t) noexcept {nullptr};
        void (*drawBuffer)(std::uint32_t) noexcept {nullptr};
        void (*drawBuffers)(std::span<const std::uint32_t>) noexcept {nullptr};
        std::uint32_t (*error)() noexcept {nullptr};

        [[nodiscard]] bool IsValid() const noexcept {
            return getInteger && getFloat && getBoolean && isEnabled && setEnabled && colorMask && bindDrawFramebuffer && drawBuffer &&
                   drawBuffers && error;
        }
    };

    /** @brief Private opaque core sync identities; polling is zero-time and never blocks the frame thread. */
    struct OpenGLSyncFunctions {
        std::uintptr_t (*fence)() noexcept {nullptr};
        std::uint32_t (*poll)(std::uintptr_t) noexcept {nullptr};
        void (*destroy)(std::uintptr_t) noexcept {nullptr};
        void (*flush)() noexcept {nullptr};

        [[nodiscard]] bool IsValid() const noexcept {
            return fence && poll && destroy && flush;
        }
    };

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
        /** Reports actual native entry-point readiness after the owning port loads dispatch. */
        bool (*isAvailable)() noexcept {nullptr};
        OpenGLViewportFunction viewport{nullptr};
        OpenGLClearColorFunction clearColor{nullptr};
        OpenGLClearFunction clear{nullptr};
        OpenGLStateFunctions state;
        OpenGLSyncFunctions sync;
        OpenGLBufferFunctions buffers;
        OpenGLVertexArrayFunctions vertexArrays;
        OpenGLTextureFunctions textures;
        OpenGLFramebufferFunctions framebuffers;

        [[nodiscard]] bool IsValid() const noexcept {
            return isAvailable != nullptr && viewport != nullptr && clearColor != nullptr && clear != nullptr && state.IsValid() &&
                   sync.IsValid();
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
