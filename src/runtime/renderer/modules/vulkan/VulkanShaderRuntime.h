#pragma once
/** @file VulkanShaderRuntime.h
 * @brief Private Vulkan shader, immutable pipeline and material descriptor owner.
 */
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"
#include "VulkanBackendModule.h"

#include <functional>
#include <memory>

namespace Horo::Render::Detail {
    struct VulkanGraphResourceState;

    /** @brief Exact resource resolved by the existing device resource registry. */
    struct VulkanMaterialResource {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        std::size_t bytes{0};
        VkImage image{VK_NULL_HANDLE};
        RenderBufferDescriptor bufferDescriptor;
        RenderTextureDescriptor textureDescriptor;
        VulkanGraphResourceState *state{nullptr};
        std::shared_ptr<const void> backing;
    };

    /** @brief Native descriptor lease; frontend pins and the submission lease protect all referenced generations.
     * @details Host destroys leases before draining/destroying the device. No destructor waits for the GPU. */
    class VulkanPreparedMaterialBinding final : public IResidentMaterialBinding {
    public:
        VulkanPreparedMaterialBinding() = default;
        VulkanPreparedMaterialBinding(const VulkanPreparedMaterialBinding &) = delete;
        VulkanPreparedMaterialBinding &operator=(const VulkanPreparedMaterialBinding &) = delete;
        ~VulkanPreparedMaterialBinding() override;
        VkDevice device{VK_NULL_HANDLE};
        PFN_vkDestroyDescriptorPool destroyPool{nullptr};
        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkPipeline pipeline{VK_NULL_HANDLE};
        VkPipelineLayout layout{VK_NULL_HANDLE};
        std::vector<VkDescriptorSet> sets;
        const void *owner{nullptr};
        std::uint64_t deviceIncarnation{0};
        RenderGraphicsPipelineState state;
        std::shared_ptr<const void> pipelineBacking;
        std::vector<std::shared_ptr<const void>> samplerBackings;
        std::vector<VulkanMaterialResource> resources;
    };

    /** @brief Synchronous load-time owner; caller serializes preparation and cancellation on the render owner thread.
     * @details Uses the resource registry's sole identity issuer. Native work and metadata are bounded (4096 objects,
     * 64 MiB artifact, four descriptor sets, 128 bindings). Shutdown follows the canonical queue drain, never waits here. */
    class VulkanShaderRuntime final {
    public:
        using Resolver = std::function<PFN_vkVoidFunction(VkDevice, const char *)>;
        using IssueIdentity = std::function<std::optional<std::uint64_t>()>;
        using ResolveResource = std::function<Result<VulkanMaterialResource>(const MaterialResidentResource &, ShaderResourceKind)>;
        VulkanShaderRuntime();
        ~VulkanShaderRuntime();
        VulkanShaderRuntime(const VulkanShaderRuntime &) = delete;
        VulkanShaderRuntime &operator=(const VulkanShaderRuntime &) = delete;
        /** @brief Attaches exact selected-device facts and borrowed registry callbacks; rolls back on missing dispatch. */
        [[nodiscard]] Result<void> Initialize(VkDevice device, const VkPhysicalDeviceProperties &properties, RenderAdapterId adapter,
                                              const Resolver &resolver, IssueIdentity issue, ResolveResource resolve,
                                              std::uint64_t deviceIncarnation = 0);
        /** @brief Releases pipeline/layout/cache/shader/sampler objects after host-owned queue drain and lease release. */
        void Shutdown() noexcept;
        [[nodiscard]] Result<RenderPipelineDeviceIdentity> QueryPipelineDeviceIdentity() const;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &, std::size_t) const;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &, std::uint64_t,
                                                                                   std::uint64_t) const;
        [[nodiscard]] Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &) const;
        [[nodiscard]] Result<std::uint64_t> CreateShaderModule(const RenderShaderModuleDescriptor &, std::span<const std::uint8_t>,
                                                               const RenderMemoryPlacement &);
        [[nodiscard]] Result<std::uint64_t> CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &, std::uint64_t, std::uint64_t,
                                                                   const RenderMemoryPlacement &);
        [[nodiscard]] Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &, const RenderMemoryPlacement &);
        void DestroyShaderModule(std::uint64_t) noexcept;
        void DestroyGraphicsPipeline(std::uint64_t) noexcept;
        void DestroySampler(std::uint64_t) noexcept;
        [[nodiscard]] Result<std::size_t> QueryMaterialParameterBufferSize(std::uint64_t, const MaterialBindingDescriptor &,
                                                                           ShaderBindingId) const;
        [[nodiscard]] Result<std::unique_ptr<IResidentMaterialBinding>> RealizeResolved(const MaterialBindingDescriptor &, std::uint64_t,
                                                                                        std::span<const MaterialResidentResource>);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
