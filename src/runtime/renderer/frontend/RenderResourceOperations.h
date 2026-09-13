#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderResourceRegistry.h"
#include "RenderResourceUploadQueue.h"

namespace Horo::Render {
    [[nodiscard]] Detail::RenderResourceIdentity Identity(RenderBufferHandle handle) noexcept;
    [[nodiscard]] Detail::RenderResourceIdentity Identity(RenderMeshHandle handle) noexcept;
    [[nodiscard]] Detail::RenderResourceIdentity Identity(RenderTextureHandle handle) noexcept;
    [[nodiscard]] Detail::RenderResourceIdentity Identity(RenderTextureViewHandle handle) noexcept;
    [[nodiscard]] Detail::RenderResourceIdentity Identity(RenderTargetHandle handle) noexcept;
    [[nodiscard]] RenderBufferHandle BufferHandle(Detail::RenderResourceIdentity identity) noexcept;
    [[nodiscard]] RenderMeshHandle MeshHandle(Detail::RenderResourceIdentity identity) noexcept;
    [[nodiscard]] RenderTextureHandle TextureHandle(Detail::RenderResourceIdentity identity) noexcept;
    [[nodiscard]] RenderTextureViewHandle TextureViewHandle(Detail::RenderResourceIdentity identity) noexcept;
    [[nodiscard]] RenderTargetHandle TargetHandle(Detail::RenderResourceIdentity identity) noexcept;
    [[nodiscard]] bool FitsBuffer(std::uint32_t elementSize, std::uint32_t elementCount, std::size_t bufferSize) noexcept;
    [[nodiscard]] Result<std::uint64_t> RealizeResourceRequest(IRenderBackend &backend, const Detail::RenderResourceRegistry &registry,
                                                               const Detail::RenderResourceUploadQueue::Request &request);
    void CompleteResourceRequest(IRenderBackend &backend, RenderMemoryBudget &memoryBudget, Detail::RenderResourceRegistry &registry,
                                 const Detail::RenderResourceUploadQueue::Request &request, const Result<std::uint64_t> &created);
}  // namespace Horo::Render
