#pragma once

/** @file VulkanReconstructionRuntime.h @brief Private Vulkan reconstruction preparation and descriptor ownership. */
#include "VulkanResourceRuntime.h"

#include <array>

namespace Horo::Render::Detail {
    /** @brief Prepared descriptor set for one slot of the current graph's canonical frame retention. */
    struct VulkanPreparedReconstruction final {
        VkPipeline pipeline{};
        VkPipelineLayout layout{};
        VkDescriptorSet images{};
    };

    /** @brief Owner-thread native reconstruction kernel realization.
     * @details Command retention pins each exact kernel until its frame slot retires. Descriptor sets and their
     * pipeline are allocated cold. Shutdown revokes still-borrowed kernel handles only after command drain.
     */
    class VulkanReconstructionRuntime final {
    public:
        /** @brief Caches native functions and exact device limits for an optional reconstruction encoder. */
        void Initialize(VkDevice device, const VkPhysicalDeviceLimits &limits, std::uint32_t frames,
                        const VulkanShaderRuntime::Resolver &resolve);

        /** @brief Reports whether this selected device can realize the exact closed family. */
        [[nodiscard]] bool Supported() const noexcept;

        /** @brief Realizes an actual cook artifact and descriptor slots before frame recording. */
        [[nodiscard]] Result<std::shared_ptr<IResidentNativeReconstructionKernel>> Realize(const CookedNativeReconstructionKernel &cooked);

        /** @brief Resolves exact graph views and updates only the currently unsubmitted frame descriptor slot. */
        [[nodiscard]] Result<VulkanPreparedReconstruction> Prepare(const RenderGraphNativeReconstruction &work,
                                                                   const RenderGraphExecutionRequest &request,
                                                                   std::span<const VulkanGraphResource> textures,
                                                                   const VulkanResourceRuntime &registry, VulkanCommandRetention &retention,
                                                                   std::size_t frame, std::size_t pass);

        /** @brief Records one already-prepared dispatch without any host memory allocation or synchronization. */
        void Encode(VkCommandBuffer command, const NativeReconstructionDispatch &constants,
                    const VulkanPreparedReconstruction &prepared) const noexcept;

        /** @brief Revokes native objects after all command and frame retained references are terminal. */
        void Reset() noexcept;

    private:
        struct Dispatch final {
            PFN_vkCreateShaderModule createModule{};
            PFN_vkDestroyShaderModule destroyModule{};
            PFN_vkCreateDescriptorSetLayout createSetLayout{};
            PFN_vkDestroyDescriptorSetLayout destroySetLayout{};
            PFN_vkCreatePipelineLayout createLayout{};
            PFN_vkDestroyPipelineLayout destroyLayout{};
            PFN_vkCreateComputePipelines createPipeline{};
            PFN_vkDestroyPipeline destroyPipeline{};
            PFN_vkCreateDescriptorPool createPool{};
            PFN_vkDestroyDescriptorPool destroyPool{};
            PFN_vkAllocateDescriptorSets allocateSets{};
            PFN_vkUpdateDescriptorSets updateSets{};
            PFN_vkCmdBindPipeline bindPipeline{};
            PFN_vkCmdBindDescriptorSets bindSets{};
            PFN_vkCmdPushConstants pushConstants{};
            PFN_vkCmdDispatch dispatch{};
        };
        class Kernel;

        // One current and one cold replacement for each of the three shader entries.
        static constexpr auto MaximumKernels = 2 * (static_cast<std::size_t>(NativeReconstructionEntry::TemporalHistory) + 1U);
        static constexpr auto MaximumPasses = RenderGraphNativeReconstruction::MaximumPassesPerFrame;
        std::array<std::weak_ptr<Kernel>, MaximumKernels> kernels_;
        VkDevice device_{};
        Dispatch native_;
        FramebufferExtent maxGroups_;
        std::uint32_t frames_{};
    };
}  // namespace Horo::Render::Detail
