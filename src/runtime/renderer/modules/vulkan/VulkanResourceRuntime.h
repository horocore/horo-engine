#pragma once

#include "VulkanBackendModule.h"
#include "VulkanCommandRuntime.h"
#include "VulkanShaderRuntime.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>

namespace Horo::Render::Detail {
    /** @brief Actual native generation state committed only after successful queue submission. */
    struct VulkanGraphResourceState {
        RenderGraphLogicalState logical;
        RenderTimelinePoint completion;
        bool hasNativeState{false};
        bool contentsDefined{false};
    };

    /** @brief Device-private graph binding captured before recording, with an exact immutable descriptor. */
    struct VulkanGraphResource {
        RenderGraphResourceKind kind{RenderGraphResourceKind::Buffer};
        VkBuffer buffer{VK_NULL_HANDLE};
        VkImage image{VK_NULL_HANDLE};
        VkImageView colorView{VK_NULL_HANDLE};
        RenderBufferDescriptor bufferDescriptor;
        RenderTextureDescriptor textureDescriptor;
        VulkanGraphResourceState *state{nullptr};
    };

    /** @brief Captured whole-image offscreen attachments; command retention owns their native backing. */
    struct VulkanMaterialTarget {
        VulkanGraphResource color;
        VulkanGraphResource depth;
        VkImageView colorView{VK_NULL_HANDLE};
        VkImageView depthView{VK_NULL_HANDLE};
        FramebufferExtent extent;
    };

    /** @brief Exact MeshVertex/index source snapshot, independent of registry discovery after capture. */
    struct VulkanMaterialMesh {
        VulkanGraphResource vertex;
        VulkanGraphResource index;
        RenderMeshDescriptor descriptor;
    };

    /** @brief Device-owned Vulkan resource allocator and identity registry. */
    class VulkanResourceRuntime final : public IRenderResourceBackend, public IMaterialBindingBackend {
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
        /** @brief Attaches the shader owner to this registry after native device creation. */
        [[nodiscard]] Result<void> InitializeShaders(const VkPhysicalDeviceProperties &properties, RenderAdapterId adapter,
                                                     const VulkanShaderRuntime::Resolver &resolver, std::uint64_t deviceIncarnation = 0);
        [[nodiscard]] Result<RenderPipelineDeviceIdentity> QueryPipelineDeviceIdentity() const override;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &,
                                                                               std::size_t) const override;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &, std::uint64_t,
                                                                                   std::uint64_t) const override;
        [[nodiscard]] Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &) const override;
        [[nodiscard]] Result<std::uint64_t> CreateShaderModule(const RenderShaderModuleDescriptor &, std::span<const std::uint8_t>,
                                                               const RenderMemoryPlacement &) override;
        [[nodiscard]] Result<std::uint64_t> CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &, std::uint64_t, std::uint64_t,
                                                                   const RenderMemoryPlacement &) override;
        [[nodiscard]] Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &, const RenderMemoryPlacement &) override;
        void DestroyShaderModule(std::uint64_t) noexcept override;
        void DestroyGraphicsPipeline(std::uint64_t) noexcept override;
        void DestroySampler(std::uint64_t) noexcept override;
        [[nodiscard]] Result<std::size_t> QueryMaterialParameterBufferSize(std::uint64_t, const MaterialBindingDescriptor &,
                                                                           ShaderBindingId) const override;
        [[nodiscard]] Result<std::unique_ptr<IResidentMaterialBinding>> RealizeResolved(const MaterialBindingDescriptor &, std::uint64_t,
                                                                                        std::span<const MaterialResidentResource>) override;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override;
        [[nodiscard]] Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &bufferDescriptor,
                                                         std::span<const std::byte> bufferInitialData,
                                                         const RenderMemoryPlacement &bufferPlacement,
                                                         RenderResourceNativeLifetime lifetime = {}) override;
        [[nodiscard]] Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertexBuffer,
                                                       std::uint64_t indexBuffer, RenderResourceNativeLifetime lifetime = {}) override;
        [[nodiscard]] Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &textureDescriptor,
                                                          std::span<const std::byte> textureInitialData,
                                                          const RenderMemoryPlacement &texturePlacement,
                                                          RenderResourceNativeLifetime lifetime = {}) override;
        [[nodiscard]] Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture,
                                                              RenderResourceNativeLifetime lifetime = {}) override;
        [[nodiscard]] Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t colorAttachment,
                                                               std::uint64_t depthAttachment,
                                                               RenderResourceNativeLifetime lifetime = {}) override;
        void DestroyBuffer(std::uint64_t backendInstance) noexcept override;
        void DestroyMesh(std::uint64_t backendInstance) noexcept override;
        void DestroyTexture(std::uint64_t backendInstance) noexcept override;
        void DestroyTextureView(std::uint64_t backendInstance) noexcept override;
        void DestroyRenderTarget(std::uint64_t backendInstance) noexcept override;
        void ShutdownResources() noexcept;
        /** @brief Captures whole single-sample attachments and retains the target's transitive backing.
         * @param identity Exact registry target. @param retention Prepared command pin storage.
         * @return Native snapshot or typed stale/unsupported/capacity failure before recording. */
        [[nodiscard]] Result<VulkanMaterialTarget> CaptureMaterialTarget(std::uint64_t identity, VulkanCommandRetention &retention);
        /** @brief Captures exact indexed mesh buffers and retains all three native registry owners.
         * @param identity Exact registry mesh. @param retention Prepared command pin storage.
         * @return Native snapshot or typed stale/capacity failure before recording. */
        [[nodiscard]] Result<VulkanMaterialMesh> CaptureMaterialMesh(std::uint64_t identity, VulkanCommandRetention &retention);
        /** @brief Captures exact native namespaces and pins actual resources into preallocated graph retention.
         * @param request Portable-validated exact frontend resource mapping.
         * @param destination Prepared snapshot storage, at least graph resource count.
         * @param retention Free owner-thread slot with prepared resource-reference capacity.
         * @return Success or typed stale/unsupported/capacity failure, before native commands. */
        [[nodiscard]] Result<void> CaptureGraphResources(const RenderGraphExecutionRequest &request,
                                                         std::span<VulkanGraphResource> destination,
                                                         VulkanCommandRetention &retention) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        VulkanShaderRuntime shaders_;
    };
}  // namespace Horo::Render::Detail
