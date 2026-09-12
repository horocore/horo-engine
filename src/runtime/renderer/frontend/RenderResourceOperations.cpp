#include "RenderResourceOperations.h"

#include "RenderFrontendErrors.h"

#include <limits>
#include <string>
#include <utility>

namespace Horo::Render {
    namespace {
        using UploadRequest = Detail::RenderResourceUploadQueue::Request;
        using UploadRequestKind = Detail::RenderResourceUploadQueue::RequestKind;

        [[nodiscard]] Detail::RenderResourceClass ResourceClassFor(const UploadRequestKind kind) noexcept {
            switch (kind) {
                case UploadRequestKind::Buffer:
                    return Detail::RenderResourceClass::Buffer;
                case UploadRequestKind::Mesh:
                    return Detail::RenderResourceClass::Mesh;
                case UploadRequestKind::Texture:
                    return Detail::RenderResourceClass::Texture;
                case UploadRequestKind::TextureView:
                    return Detail::RenderResourceClass::TextureView;
                case UploadRequestKind::RenderTarget:
                    return Detail::RenderResourceClass::RenderTarget;
            }
            return Detail::RenderResourceClass::Buffer;
        }

        [[nodiscard]] Result<std::uint64_t> RealizeMeshRequest(IRenderBackend &backend, const Detail::RenderResourceRegistry &registry,
                                                               const RenderMeshDescriptor &descriptor) {
            const auto vertex = registry.BackendInstance(Detail::RenderResourceClass::Buffer, Identity(descriptor.vertexBuffer));
            if (vertex.HasError()) {
                return Result<std::uint64_t>::Failure(vertex.ErrorValue());
            }
            const auto index = registry.BackendInstance(Detail::RenderResourceClass::Buffer, Identity(descriptor.indexBuffer));
            if (index.HasError()) {
                return Result<std::uint64_t>::Failure(index.ErrorValue());
            }
            return backend.CreateMesh(descriptor, vertex.Value(), index.Value());
        }

        [[nodiscard]] Result<std::uint64_t> RealizeTextureViewRequest(IRenderBackend &backend,
                                                                      const Detail::RenderResourceRegistry &registry,
                                                                      const RenderTextureViewDescriptor &descriptor) {
            const auto texture = registry.BackendInstance(Detail::RenderResourceClass::Texture, Identity(descriptor.texture));
            if (texture.HasError())
                return Result<std::uint64_t>::Failure(texture.ErrorValue());
            return backend.CreateTextureView(descriptor, texture.Value());
        }

        [[nodiscard]] Result<std::uint64_t> RealizeRenderTargetRequest(IRenderBackend &backend,
                                                                       const Detail::RenderResourceRegistry &registry,
                                                                       const RenderTargetDescriptor &descriptor) {
            std::uint64_t colorInstance = 0;
            if (descriptor.colorAttachment.IsValid()) {
                const auto color = registry.BackendInstance(Detail::RenderResourceClass::TextureView, Identity(descriptor.colorAttachment));
                if (color.HasError())
                    return Result<std::uint64_t>::Failure(color.ErrorValue());
                colorInstance = color.Value();
            }
            std::uint64_t depthInstance = 0;
            if (descriptor.depthAttachment.IsValid()) {
                const auto depth = registry.BackendInstance(Detail::RenderResourceClass::TextureView, Identity(descriptor.depthAttachment));
                if (depth.HasError())
                    return Result<std::uint64_t>::Failure(depth.ErrorValue());
                depthInstance = depth.Value();
            }
            return backend.CreateRenderTarget(descriptor, colorInstance, depthInstance);
        }

        void DestroyResourceInstance(IRenderBackend &backend, const Detail::RenderResourceClass resourceClass,
                                     const std::uint64_t backendInstance) noexcept {
            using enum Detail::RenderResourceClass;
            if (resourceClass == Buffer) {
                backend.DestroyBuffer(backendInstance);
                return;
            }
            if (resourceClass == Mesh) {
                backend.DestroyMesh(backendInstance);
                return;
            }
            if (resourceClass == Texture) {
                backend.DestroyTexture(backendInstance);
                return;
            }
            if (resourceClass == TextureView) {
                backend.DestroyTextureView(backendInstance);
                return;
            }
            backend.DestroyRenderTarget(backendInstance);
        }
    }  // namespace

    Detail::RenderResourceIdentity Identity(const RenderBufferHandle handle) noexcept {
        return {handle.owner, handle.slot, handle.generation};
    }

    Detail::RenderResourceIdentity Identity(const RenderMeshHandle handle) noexcept {
        return {handle.owner, handle.slot, handle.generation};
    }

    Detail::RenderResourceIdentity Identity(const RenderTextureHandle handle) noexcept {
        return {handle.owner, handle.slot, handle.generation};
    }

    Detail::RenderResourceIdentity Identity(const RenderTextureViewHandle handle) noexcept {
        return {handle.owner, handle.slot, handle.generation};
    }

    Detail::RenderResourceIdentity Identity(const RenderTargetHandle handle) noexcept {
        return {handle.owner, handle.slot, handle.generation};
    }

    RenderBufferHandle BufferHandle(const Detail::RenderResourceIdentity identity) noexcept {
        return {identity.owner, identity.slot, identity.generation};
    }

    RenderMeshHandle MeshHandle(const Detail::RenderResourceIdentity identity) noexcept {
        return {identity.owner, identity.slot, identity.generation};
    }

    RenderTextureHandle TextureHandle(const Detail::RenderResourceIdentity identity) noexcept {
        return {identity.owner, identity.slot, identity.generation};
    }

    RenderTextureViewHandle TextureViewHandle(const Detail::RenderResourceIdentity identity) noexcept {
        return {identity.owner, identity.slot, identity.generation};
    }

    RenderTargetHandle TargetHandle(const Detail::RenderResourceIdentity identity) noexcept {
        return {identity.owner, identity.slot, identity.generation};
    }

    bool FitsBuffer(const std::uint32_t elementSize, const std::uint32_t elementCount, const std::size_t bufferSize) noexcept {
        return elementSize != 0 && elementCount <= std::numeric_limits<std::size_t>::max() / elementSize &&
               static_cast<std::size_t>(elementSize) * elementCount <= bufferSize;
    }

    Result<std::uint64_t> RealizeResourceRequest(IRenderBackend &backend, const Detail::RenderResourceRegistry &registry,
                                                 const UploadRequest &request) {
        try {
            using enum UploadRequestKind;
            switch (request.kind) {
                case Buffer:
                    return backend.CreateBuffer(request.buffer, request.initialData, request.memoryPlacement);
                case Mesh:
                    return RealizeMeshRequest(backend, registry, request.mesh);
                case Texture:
                    return backend.CreateTexture(request.texture, request.initialData, request.memoryPlacement);
                case TextureView:
                    return RealizeTextureViewRequest(backend, registry, request.textureView);
                case RenderTarget:
                    return RealizeRenderTargetRequest(backend, registry, request.renderTarget);
            }
            return Result<std::uint64_t>::Failure(
                MakeError(FrontendErrors::ResourceBackendException, "Renderer resource request kind is invalid."));
        } catch (...) {  // NOSONAR(cpp:S2738)
            return Result<std::uint64_t>::Failure(
                MakeError(FrontendErrors::ResourceBackendException, "Renderer backend resource realization threw an exception."));
        }
    }

    void CompleteResourceRequest(IRenderBackend &backend, RenderMemoryBudget &memoryBudget, Detail::RenderResourceRegistry &registry,
                                 const UploadRequest &request, const Result<std::uint64_t> &created) {
        const Detail::RenderResourceClass resourceClass = ResourceClassFor(request.kind);
        if (created.HasError()) {
            if (request.memoryReservation.IsValid())
                static_cast<void>(memoryBudget.Cancel(request.memoryReservation));
            static_cast<void>(registry.Fail(resourceClass, request.identity, created.ErrorValue()));
            return;
        }

        std::optional<RenderMemoryAllocation> allocation;
        if (request.memoryReservation.IsValid()) {
            auto committed = memoryBudget.Commit(request.memoryReservation);
            if (committed.HasError()) {
                DestroyResourceInstance(backend, resourceClass, created.Value());
                static_cast<void>(memoryBudget.Cancel(request.memoryReservation));
                static_cast<void>(registry.Fail(resourceClass, request.identity, committed.ErrorValue()));
                return;
            }
            allocation = committed.Value();
        }
        const std::optional<RenderMemoryAllocationId> allocationId = allocation.has_value() ? std::optional{allocation->id} : std::nullopt;
        if (const Result<void> published = registry.Publish(resourceClass, request.identity, created.Value(), allocationId);
            published.HasError()) {
            DestroyResourceInstance(backend, resourceClass, created.Value());
            if (allocation.has_value()) {
                static_cast<void>(memoryBudget.BeginRetire(allocation->id));
                static_cast<void>(memoryBudget.AcknowledgeRetirement(allocation->id));
            }
            static_cast<void>(registry.Fail(resourceClass, request.identity, published.ErrorValue()));
            return;
        }
        if (request.replacedMesh.has_value()) {
            static_cast<void>(registry.Release(Detail::RenderResourceClass::Mesh, *request.replacedMesh));
        }
    }
}  // namespace Horo::Render
