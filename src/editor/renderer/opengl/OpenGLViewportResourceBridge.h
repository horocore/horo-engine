#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"

namespace Horo::Editor {
    /** @brief Matching editor integration that consumes generic handles without exposing native objects. */
    class OpenGLViewportResourceBridge final {
    public:
        /** @brief Returns the private OpenGL object for one ready generic buffer. */
        [[nodiscard]] static Result<std::uint32_t> ResolveBuffer(const Render::RenderFrontend &frontend, Render::RenderBufferHandle buffer);

        /** @brief Binds a ready generic mesh's backend-private vertex input object. */
        [[nodiscard]] static Result<void> BindMesh(const Render::RenderFrontend &frontend, Render::RenderMeshHandle mesh);

        /** @brief Binds a ready generic offscreen render target. */
        [[nodiscard]] static Result<void> BindRenderTarget(const Render::RenderFrontend &frontend, Render::RenderTargetHandle target);

        /** @brief Binds a ready generic texture view to an OpenGL texture unit slot. */
        [[nodiscard]] static Result<void> BindTexture(const Render::RenderFrontend &frontend, Render::RenderTextureViewHandle view,
                                                      std::uint32_t unit);

        /** @brief Returns the opaque GUI identity derived from a ready generic texture view. */
        [[nodiscard]] static Result<std::uintptr_t> EditorImageIdentity(const Render::RenderFrontend &frontend,
                                                                        Render::RenderTextureViewHandle view);
    };
}  // namespace Horo::Editor
