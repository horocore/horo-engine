#include "OpenGLViewportResourceBridge.h"

#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"
#include "runtime/renderer/modules/opengl/OpenGLRenderBackendErrors.h"

#include <glad/gl.h>
#include <limits>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Result<std::uint32_t> ResolveOpenGLObject(const Render::RenderFrontend &frontend,
                                                                const Result<std::uint64_t> &resolved) {
            if (frontend.Capabilities().backend != Render::RenderBackendId{"opengl"}) {
                return Result<std::uint32_t>::Failure(MakeError(Render::OpenGLBackendErrors::UnsupportedResourceOperation,
                                                                "OpenGL editor bridge received a non-OpenGL frontend."));
            }
            if (resolved.HasError())
                return Result<std::uint32_t>::Failure(resolved.ErrorValue());
            if (resolved.Value() == 0 || resolved.Value() > std::numeric_limits<std::uint32_t>::max()) {
                return Result<std::uint32_t>::Failure(MakeError(Render::OpenGLBackendErrors::UnsupportedResourceOperation,
                                                                "OpenGL resource identity is outside the native object range."));
            }
            return Result<std::uint32_t>::Success(static_cast<std::uint32_t>(resolved.Value()));
        }
    }  // namespace

    /** @copydoc OpenGLViewportResourceBridge::ResolveBuffer */
    Result<std::uint32_t> OpenGLViewportResourceBridge::ResolveBuffer(const Render::RenderFrontend &frontend,
                                                                      const Render::RenderBufferHandle buffer) {
        return ResolveOpenGLObject(frontend, Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, buffer));
    }

    /** @copydoc OpenGLViewportResourceBridge::BindMesh */
    Result<void> OpenGLViewportResourceBridge::BindMesh(const Render::RenderFrontend &frontend, const Render::RenderMeshHandle mesh) {
        const auto object = ResolveOpenGLObject(frontend, Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, mesh));
        if (object.HasError())
            return Result<void>::Failure(object.ErrorValue());
        glBindVertexArray(object.Value());
        return Result<void>::Success();
    }

    /** @copydoc OpenGLViewportResourceBridge::BindRenderTarget */
    Result<void> OpenGLViewportResourceBridge::BindRenderTarget(const Render::RenderFrontend &frontend,
                                                                const Render::RenderTargetHandle target) {
        const auto object = ResolveOpenGLObject(frontend, Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, target));
        if (object.HasError())
            return Result<void>::Failure(object.ErrorValue());
        glBindFramebuffer(GL_FRAMEBUFFER, object.Value());
        return Result<void>::Success();
    }

    /** @copydoc OpenGLViewportResourceBridge::BindTexture */
    Result<void> OpenGLViewportResourceBridge::BindTexture(const Render::RenderFrontend &frontend,
                                                           const Render::RenderTextureViewHandle view, const std::uint32_t unit) {
        if (unit >= 32)
            return Result<void>::Failure(MakeError(Render::OpenGLBackendErrors::UnsupportedResourceOperation,
                                                   "OpenGL editor texture unit is outside the supported bridge range."));
        const auto object = ResolveOpenGLObject(frontend, Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, view));
        if (object.HasError())
            return Result<void>::Failure(object.ErrorValue());
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, object.Value());
        return Result<void>::Success();
    }

    /** @copydoc OpenGLViewportResourceBridge::EditorImageIdentity */
    Result<std::uintptr_t> OpenGLViewportResourceBridge::EditorImageIdentity(const Render::RenderFrontend &frontend,
                                                                             const Render::RenderTextureViewHandle view) {
        const auto object = ResolveOpenGLObject(frontend, Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, view));
        if (object.HasError())
            return Result<std::uintptr_t>::Failure(object.ErrorValue());
        return Result<std::uintptr_t>::Success(object.Value());
    }
}  // namespace Horo::Editor
