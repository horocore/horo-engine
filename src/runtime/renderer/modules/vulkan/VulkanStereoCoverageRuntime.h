#pragma once

/** @file VulkanStereoCoverageRuntime.h
 * @brief Private selected Vulkan owner of immutable stereo reduction descriptors and dispatch.
 */
#include "VulkanResourceRuntime.h"

#include <VulkanEditorGraphicsBridge.h>

namespace Horo::Render::Detail {
    /** @brief Native dispatch facts pinned by the accepted graph retention lease. */
    struct VulkanPreparedStereoCoverage final {
        VkPipeline pipeline{};
        VkPipelineLayout layout{};
        VkDescriptorSet descriptors{};
        VkBuffer result{};
        FramebufferExtent groups;
    };

    /** @brief Cold pipeline realization and allocation-free recording for one closed compute program. */
    class VulkanStereoCoverageRuntime final {
    public:
        /** @brief Caches available device entry points and actual compute limits without requiring this optional route. */
        void Initialize(VkDevice device, const VkPhysicalDeviceLimits &limits,
                        const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver);
        /** @brief Reports whether this selected device admitted the fixed shader's workgroup and native entry points. */
        [[nodiscard]] bool Supported() const noexcept;
        /** @brief Cold-validates the exact resources and realizes immutable Vulkan descriptors over them. */
        [[nodiscard]] Result<std::shared_ptr<IResidentStereoCoverageReducer>> Realize(const VulkanStereoCoveragePreparation &request,
                                                                                      VulkanResourceRuntime &registry);
        /** @brief Matches each authored graph handle against the cold native generation and retains the pipeline. */
        [[nodiscard]] Result<VulkanPreparedStereoCoverage> Prepare(const RenderGraphStereoCoverageReduction &work,
                                                                   const RenderGraphExecutionRequest &request,
                                                                   std::span<const VulkanGraphResource> resources, RenderQueueId queue,
                                                                   VulkanCommandRetention &retention) const;
        /** @brief Records the admitted compute dispatch and its storage-to-host visibility barrier. */
        void Encode(VkCommandBuffer command, const VulkanPreparedStereoCoverage &prepared) const noexcept;
        /** @brief Destroys every resident pipeline while its drained device owner still lives. */
        void Reset() noexcept;

    private:
        class Kernel;

        struct Dispatch final {
            PFN_vkCreateShaderModule createModule{};
            PFN_vkDestroyShaderModule destroyModule{};
            PFN_vkCreateDescriptorSetLayout createDescriptorLayout{};
            PFN_vkDestroyDescriptorSetLayout destroyDescriptorLayout{};
            PFN_vkCreatePipelineLayout createLayout{};
            PFN_vkDestroyPipelineLayout destroyLayout{};
            PFN_vkCreateComputePipelines createPipeline{};
            PFN_vkDestroyPipeline destroyPipeline{};
            PFN_vkCreateDescriptorPool createPool{};
            PFN_vkDestroyDescriptorPool destroyPool{};
            PFN_vkAllocateDescriptorSets allocateSet{};
            PFN_vkUpdateDescriptorSets updateSets{};
            PFN_vkCmdBindPipeline bindPipeline{};
            PFN_vkCmdBindDescriptorSets bindSets{};
            PFN_vkCmdDispatch dispatch{};
            PFN_vkCmdPipelineBarrier2 barrier{};
        } native_;

        VkDevice device_{};
        FramebufferExtent maxGroups_;
        std::array<std::weak_ptr<Kernel>, 4> kernels_;
    };
}  // namespace Horo::Render::Detail
