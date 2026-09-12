#include "MetalViewportResourceBridge.h"

#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"
#include "runtime/renderer/modules/metal/MetalRenderBackendErrors.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"

#include <limits>

namespace Horo::Editor {
    namespace {
        template <typename Instance>
        [[nodiscard]] Result<Instance *> ResolveMetalInstance(const Render::RenderFrontend &frontend,
                                                              const Result<std::uint64_t> &resolved) {
            if (frontend.Capabilities().backend != Render::RenderBackendId{"metal"}) {
                return Result<Instance *>::Failure(MakeError(Render::MetalBackendErrors::UnsupportedResourceOperation,
                                                             "Metal editor bridge received a non-Metal frontend."));
            }
            if (resolved.HasError())
                return Result<Instance *>::Failure(resolved.ErrorValue());
            if (resolved.Value() == 0 || resolved.Value() > std::numeric_limits<std::uintptr_t>::max()) {
                return Result<Instance *>::Failure(MakeError(Render::MetalBackendErrors::ResourceIdentityInvalid,
                                                             "Metal resource identity is outside the native pointer range."));
            }
            return Result<Instance *>::Success(reinterpret_cast<Instance *>(static_cast<std::uintptr_t>(resolved.Value())));
        }
    }  // namespace

    Result<MetalViewportMeshBinding> MetalViewportResourceBridge::ResolveMesh(const Render::RenderFrontend &frontend,
                                                                              const Render::RenderMeshHandle mesh) {
        const auto instance =
            ResolveMetalInstance<Render::Detail::MetalMeshInstance>(frontend,
                                                                    Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend,
                                                                                                                                  mesh));
        if (instance.HasError())
            return Result<MetalViewportMeshBinding>::Failure(instance.ErrorValue());
        return Result<MetalViewportMeshBinding>::Success({
            .vertexBuffer = (__bridge void *)instance.Value()->vertexBuffer,
            .indexBuffer = (__bridge void *)instance.Value()->indexBuffer,
        });
    }

    Result<void *> MetalViewportResourceBridge::ResolveBuffer(const Render::RenderFrontend &frontend,
                                                              const Render::RenderBufferHandle buffer) {
        const auto instance = ResolveMetalInstance<
            Render::Detail::MetalBufferInstance>(frontend, Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, buffer));
        if (instance.HasError())
            return Result<void *>::Failure(instance.ErrorValue());
        return Result<void *>::Success((__bridge void *)instance.Value()->buffer);
    }

    Result<MetalViewportTargetBinding> MetalViewportResourceBridge::ResolveRenderTarget(const Render::RenderFrontend &frontend,
                                                                                        const Render::RenderTargetHandle target) {
        const auto instance = ResolveMetalInstance<
            Render::Detail::MetalRenderTargetInstance>(frontend,
                                                       Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, target));
        if (instance.HasError())
            return Result<MetalViewportTargetBinding>::Failure(instance.ErrorValue());
        return Result<MetalViewportTargetBinding>::Success({
            .colorTexture = (__bridge void *)instance.Value()->colorTexture,
            .depthTexture = (__bridge void *)instance.Value()->depthTexture,
        });
    }

    Result<void *> MetalViewportResourceBridge::ResolveTexture(const Render::RenderFrontend &frontend,
                                                               const Render::RenderTextureViewHandle view) {
        const auto instance = ResolveMetalInstance<
            Render::Detail::MetalTextureViewInstance>(frontend,
                                                      Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, view));
        if (instance.HasError())
            return Result<void *>::Failure(instance.ErrorValue());
        return Result<void *>::Success((__bridge void *)instance.Value()->texture);
    }

    Result<std::uintptr_t> MetalViewportResourceBridge::EditorImageIdentity(const Render::RenderFrontend &frontend,
                                                                            const Render::RenderTextureViewHandle view) {
        const auto texture = ResolveTexture(frontend, view);
        if (texture.HasError())
            return Result<std::uintptr_t>::Failure(texture.ErrorValue());
        return Result<std::uintptr_t>::Success(reinterpret_cast<std::uintptr_t>(texture.Value()));
    }
}  // namespace Horo::Editor
