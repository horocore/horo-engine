#include "MetalViewportResourceBridge.h"

#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"
#include "runtime/renderer/modules/metal/MetalRenderBackendErrors.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"

#include <cstring>
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

        bool NativeUseComplete(id<MTLCommandBuffer> commands) noexcept {
            return commands == nil || commands.status == MTLCommandBufferStatusCompleted || commands.status == MTLCommandBufferStatusError;
        }

        Result<void> ViewportBufferCompletion(const Render::Detail::MetalResidentUse &use) {
            if (!NativeUseComplete(use.last) || !NativeUseComplete(use.previous))
                return Result<void>::Failure(MakeError(Render::LightCullingErrors::Pending));
            if (use.last.status == MTLCommandBufferStatusError || use.previous.status == MTLCommandBufferStatusError)
                return Result<void>::Failure(MakeError(Render::MetalBackendErrors::CommandSubmissionFailed));
            return Result<void>::Success();
        }

        id<MTLCommandBuffer> MatchingNativeCommands(const Render::Detail::MetalResidentUse &use, void *identity) noexcept {
            if ((__bridge void *)use.last == identity)
                return use.last;
            return (__bridge void *)use.previous == identity ? use.previous : nil;
        }

        Result<Render::Detail::MetalBufferInstance *> WritableViewportBuffer(const Render::RenderFrontend &frontend,
                                                                             const MetalViewportBufferUpdate &update) {
            using Outcome = Result<Render::Detail::MetalBufferInstance *>;
            const auto resolved = ResolveMetalInstance<
                Render::Detail::MetalBufferInstance>(frontend,
                                                     Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend,
                                                                                                                   update.buffer));
            if (resolved.HasError())
                return Outcome::Failure(resolved.ErrorValue());
            const auto *buffer = resolved.Value();
            if (buffer->use.retired || buffer->buffer.storageMode != MTLStorageModeShared || !buffer->buffer.contents ||
                update.bytes.empty() || update.offset > buffer->buffer.length ||
                update.bytes.size() > buffer->buffer.length - update.offset)
                return Outcome::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
            if (const auto complete = ViewportBufferCompletion(buffer->use); complete.HasError())
                return Outcome::Failure(complete.ErrorValue());
            return resolved;
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

    /** @copydoc MetalViewportResourceBridge::ColorBufferReadiness */
    Result<EditorViewportColorReadiness> MetalViewportResourceBridge::ColorBufferReadiness(const Render::RenderFrontend &frontend,
                                                                                           Render::RenderBufferHandle buffer) {
        using Outcome = Result<EditorViewportColorReadiness>;
        const auto boundary = Render::Detail::RenderFrontendResourceAccess::RequirePreparationBoundary(frontend);
        if (boundary.HasError())
            return Outcome::Failure(boundary.ErrorValue());
        const auto resolved = ResolveMetalInstance<
            Render::Detail::MetalBufferInstance>(frontend, Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, buffer));
        if (resolved.HasError())
            return Outcome::Failure(resolved.ErrorValue());
        const auto *native = resolved.Value();
        if (native->use.retired || native->deviceIncarnation != frontend.Capabilities().support.deviceIncarnation)
            return Outcome::Failure(MakeError(Render::MetalBackendErrors::ResourceIdentityInvalid));
        const auto complete = ViewportBufferCompletion(native->use);
        if (complete.HasValue())
            return Outcome::Success(EditorViewportColorReadiness::Ready);
        if (complete.ErrorValue().code.Value() == Render::LightCullingErrors::Pending.code.Value())
            return Outcome::Success(EditorViewportColorReadiness::Pending);
        return Outcome::Failure(complete.ErrorValue());
    }

    Result<void *> MetalViewportResourceBridge::ResolveBuffer(const Render::RenderFrontend &frontend,
                                                              const Render::RenderBufferHandle buffer) {
        const auto instance = ResolveMetalInstance<
            Render::Detail::MetalBufferInstance>(frontend, Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, buffer));
        if (instance.HasError())
            return Result<void *>::Failure(instance.ErrorValue());
        return Result<void *>::Success((__bridge void *)instance.Value()->buffer);
    }

    Result<void> MetalViewportResourceBridge::UpdateBuffers(const Render::RenderFrontend &frontend,
                                                            const Render::MetalEditorGraphicsBridge &bridge,
                                                            const std::span<const MetalViewportBufferUpdate> updates) {
        if (bridge.CurrentCommandBuffer() != nullptr)
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        for (const auto &update : updates)
            if (const auto ready = WritableViewportBuffer(frontend, update); ready.HasError())
                return Result<void>::Failure(ready.ErrorValue());
        // Calls are serialized on the graphics owner; all actual native readers were checked above.
        for (const auto &update : updates) {
            const auto native = WritableViewportBuffer(frontend, update).Value();
            std::memcpy(static_cast<std::byte *>(native->buffer.contents) + update.offset, update.bytes.data(), update.bytes.size());
        }
        return Result<void>::Success();
    }

    Result<std::span<const std::byte>> MetalViewportResourceBridge::CompletedBufferBytes(const Render::RenderFrontend &frontend,
                                                                                         const Render::RenderBufferHandle buffer,
                                                                                         void *commands) {
        using Outcome = Result<std::span<const std::byte>>;
        const auto resolved = ResolveMetalInstance<
            Render::Detail::MetalBufferInstance>(frontend, Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, buffer));
        if (resolved.HasError())
            return Outcome::Failure(resolved.ErrorValue());
        const auto *native = resolved.Value();
        id<MTLCommandBuffer> expected = MatchingNativeCommands(native->use, commands);
        if (!expected)
            return Outcome::Failure(MakeError(Render::LightCullingErrors::Pending));
        if (expected.status == MTLCommandBufferStatusError)
            return Outcome::Failure(MakeError(Render::MetalBackendErrors::CommandSubmissionFailed));
        if (expected.status != MTLCommandBufferStatusCompleted)
            return Outcome::Failure(MakeError(Render::LightCullingErrors::Pending));
        if (native->use.retired || !native->buffer.contents || native->buffer.storageMode != MTLStorageModeShared)
            return Outcome::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        return Outcome::Success({static_cast<const std::byte *>(native->buffer.contents), native->buffer.length});
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

    Result<void> MetalViewportResourceBridge::RetainTextureView(const Render::RenderFrontend &frontend,
                                                                Render::MetalEditorGraphicsBridge &bridge,
                                                                const Render::RenderTextureViewHandle view) {
        const auto instance = Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, view);
        if (instance.HasError())
            return Result<void>::Failure(instance.ErrorValue());
        return bridge.RetainEditorTextureView(instance.Value());
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
