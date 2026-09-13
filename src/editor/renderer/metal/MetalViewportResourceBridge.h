#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"

namespace Horo::Editor {
    struct MetalViewportMeshBinding {
        void *vertexBuffer{nullptr};
        void *indexBuffer{nullptr};
    };

    struct MetalViewportTargetBinding {
        void *colorTexture{nullptr};
        void *depthTexture{nullptr};
    };

    /** @brief Resolves typed frontend resources into Metal objects at the private editor/backend boundary. */
    class MetalViewportResourceBridge final {
    public:
        [[nodiscard]] static Result<void *> ResolveBuffer(const Render::RenderFrontend &frontend, Render::RenderBufferHandle buffer);
        [[nodiscard]] static Result<MetalViewportMeshBinding> ResolveMesh(const Render::RenderFrontend &frontend,
                                                                          Render::RenderMeshHandle mesh);
        [[nodiscard]] static Result<MetalViewportTargetBinding> ResolveRenderTarget(const Render::RenderFrontend &frontend,
                                                                                    Render::RenderTargetHandle target);
        [[nodiscard]] static Result<void *> ResolveTexture(const Render::RenderFrontend &frontend, Render::RenderTextureViewHandle view);
        [[nodiscard]] static Result<std::uintptr_t> EditorImageIdentity(const Render::RenderFrontend &frontend,
                                                                        Render::RenderTextureViewHandle view);
    };
}  // namespace Horo::Editor
