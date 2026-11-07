#pragma once

#include "EditorViewportColorPipeline.h"
#include "Horo/Runtime/Render/RenderFrontend.h"

namespace Horo::Render {
    class MetalEditorGraphicsBridge;
}

namespace Horo::Editor {
    struct MetalViewportMeshBinding {
        void *vertexBuffer{nullptr};
        void *indexBuffer{nullptr};
    };

    struct MetalViewportTargetBinding {
        void *colorTexture{nullptr};
        void *depthTexture{nullptr};
    };

    struct MetalViewportBufferUpdate final {
        Render::RenderBufferHandle buffer;
        std::span<const std::byte> bytes;
        std::size_t offset{};
    };

    /** @brief Resolves typed frontend resources into Metal objects at the private editor/backend boundary. */
    class MetalViewportResourceBridge final {
    public:
        /** @brief Polls actual registered buffer uses without waiting or reading GPU contents.
         * @param frontend Existing selected owner. @param buffer Exact live frontend handle.
         * @return Ready only for successful completed native uses, Pending for in-flight work or original typed error. */
        [[nodiscard]] static Result<EditorViewportColorReadiness> ColorBufferReadiness(const Render::RenderFrontend &frontend,
                                                                                       Render::RenderBufferHandle buffer);
        [[nodiscard]] static Result<void *> ResolveBuffer(const Render::RenderFrontend &frontend, Render::RenderBufferHandle buffer);
        /** @brief Validates every actual native completion before changing any mapped per-frame draw buffer. */
        [[nodiscard]] static Result<void> UpdateBuffers(const Render::RenderFrontend &frontend,
                                                        const Render::MetalEditorGraphicsBridge &bridge,
                                                        std::span<const MetalViewportBufferUpdate> updates);
        /** @brief Borrows mapped bytes only after the exact recorded native command completed successfully. */
        [[nodiscard]] static Result<std::span<const std::byte>> CompletedBufferBytes(const Render::RenderFrontend &frontend,
                                                                                     Render::RenderBufferHandle buffer, void *commands);
        [[nodiscard]] static Result<MetalViewportMeshBinding> ResolveMesh(const Render::RenderFrontend &frontend,
                                                                          Render::RenderMeshHandle mesh);
        [[nodiscard]] static Result<MetalViewportTargetBinding> ResolveRenderTarget(const Render::RenderFrontend &frontend,
                                                                                    Render::RenderTargetHandle target);
        /** @brief Pins the actual atlas source in the selected runtime's current commit/abort use journal. */
        [[nodiscard]] static Result<void> RetainTextureView(const Render::RenderFrontend &frontend,
                                                            Render::MetalEditorGraphicsBridge &bridge,
                                                            Render::RenderTextureViewHandle view);
        [[nodiscard]] static Result<void *> ResolveTexture(const Render::RenderFrontend &frontend, Render::RenderTextureViewHandle view);
        [[nodiscard]] static Result<std::uintptr_t> EditorImageIdentity(const Render::RenderFrontend &frontend,
                                                                        Render::RenderTextureViewHandle view);
    };
}  // namespace Horo::Editor
