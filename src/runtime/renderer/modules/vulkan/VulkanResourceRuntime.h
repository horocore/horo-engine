#pragma once

#include "VulkanBackendModule.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>

namespace Horo::Render::Detail {
    /** @brief Device-owned Vulkan resource allocator and identity registry. */
    class VulkanResourceRuntime final : public IRenderResourceBackend {
    public:
        /**
         * @brief Creates an inert resource runtime with a bounded backend-private identity range.
         * @param maximumIdentity Largest non-zero identity that may be published.
         */
        explicit VulkanResourceRuntime(std::uint64_t maximumIdentity = std::numeric_limits<std::uint64_t>::max());
        ~VulkanResourceRuntime() override;
        VulkanResourceRuntime(const VulkanResourceRuntime &) = delete;
        VulkanResourceRuntime &operator=(const VulkanResourceRuntime &) = delete;

        [[nodiscard]] Result<void> Initialize(const VkPhysicalDeviceMemoryProperties &memoryProperties, VkDevice device,
                                              const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver);
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override;
        [[nodiscard]] Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &bufferDescriptor,
                                                         std::span<const std::byte> bufferInitialData,
                                                         const RenderMemoryPlacement &bufferPlacement) override;
        [[nodiscard]] Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertexBuffer,
                                                       std::uint64_t indexBuffer) override;
        [[nodiscard]] Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &textureDescriptor,
                                                          std::span<const std::byte> textureInitialData,
                                                          const RenderMemoryPlacement &texturePlacement) override;
        [[nodiscard]] Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor,
                                                              std::uint64_t texture) override;
        [[nodiscard]] Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t colorAttachment,
                                                               std::uint64_t depthAttachment) override;
        void DestroyBuffer(std::uint64_t backendInstance) noexcept override;
        void DestroyMesh(std::uint64_t backendInstance) noexcept override;
        void DestroyTexture(std::uint64_t backendInstance) noexcept override;
        void DestroyTextureView(std::uint64_t backendInstance) noexcept override;
        void DestroyRenderTarget(std::uint64_t backendInstance) noexcept override;
        void ShutdownResources() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
