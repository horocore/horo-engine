#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"

#include <memory>

namespace Horo::Render::Detail {
    /** @brief Owns Metal realizations for backend-neutral resident resources. */
    class MetalResourceRuntime final {
    public:
        MetalResourceRuntime();
        ~MetalResourceRuntime();

        MetalResourceRuntime(const MetalResourceRuntime &) = delete;
        MetalResourceRuntime &operator=(const MetalResourceRuntime &) = delete;

        void Initialize(void *device) noexcept;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const;
        [[nodiscard]] Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> initialData,
                                                         const RenderMemoryPlacement &placement);
        [[nodiscard]] Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertexBuffer,
                                                       std::uint64_t indexBuffer);
        [[nodiscard]] Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> initialData,
                                                          const RenderMemoryPlacement &placement);
        [[nodiscard]] Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture);
        [[nodiscard]] Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t colorAttachment,
                                                               std::uint64_t depthAttachment);
        void DestroyBuffer(std::uint64_t backendInstance) noexcept;
        void DestroyMesh(std::uint64_t backendInstance) noexcept;
        void DestroyTexture(std::uint64_t backendInstance) noexcept;
        void DestroyTextureView(std::uint64_t backendInstance) noexcept;
        void DestroyRenderTarget(std::uint64_t backendInstance) noexcept;
        void Shutdown() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
