#pragma once
/** @file VulkanVfxGpuRuntime.h @brief Native PreparedParticles owner over the existing Vulkan resource/command authorities. */
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "VulkanResourceRuntime.h"

namespace Horo::Render::Detail {
    class VulkanVfxGpuKernel;

    class VulkanVfxGpuRuntime final {
    public:
        VulkanVfxGpuRuntime();
        ~VulkanVfxGpuRuntime();
        [[nodiscard]] Result<void> Initialize(VkDevice, const VkPhysicalDeviceProperties &, std::uint32_t frames, std::uint64_t incarnation,
                                              std::span<const VulkanCommandQueue>, VulkanResourceRuntime &, VulkanCommandRuntime &,
                                              const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &);
        [[nodiscard]] bool Available() const noexcept;
        [[nodiscard]] Result<NativeVfxGpuPreparationResources> QueryResources(const VfxGpuSimulationDescriptor &) const;
        [[nodiscard]] Result<std::shared_ptr<IResidentVfxGpuKernel>> Programs(const CookedVfxGpuPrograms &);
        [[nodiscard]] Result<std::shared_ptr<IResidentVfxGpuSimulation>> Prepare(const NativeVfxGpuSimulationPreparation &);
        [[nodiscard]] Result<std::size_t> Poll(const std::shared_ptr<IResidentVfxGpuSimulation> &, std::span<NativeGpuVfxStepReceipt>);
        [[nodiscard]] Result<std::size_t> Copy(const std::shared_ptr<IResidentVfxGpuSimulation> &, const VfxGpuStepIdentity &,
                                               std::span<std::byte>);
        [[nodiscard]] Result<void> Discard(const std::shared_ptr<IResidentVfxGpuSimulation> &, const VfxGpuStepIdentity &);
        [[nodiscard]] Result<void> Validate(const RenderGraphExecutionRequest &, std::size_t pass, std::span<const VulkanGraphResource>);
        [[nodiscard]] Result<void> Encode(VkCommandBuffer, VulkanCommandTicket, const RenderQueueSubmission &,
                                          const RenderGraphExecutionRequest &, std::size_t pass, std::span<const VulkanGraphResource>);
        void Finish(VulkanCommandTicket, bool accepted) noexcept;
        void Uncertain(VulkanCommandTicket) noexcept;
        [[nodiscard]] Result<void> CollectRetired();
        [[nodiscard]] bool Owns(VkBuffer) const noexcept;
        void Reject(const RenderGraphExecutionRequest &) noexcept;
        void Abort() noexcept;
        void Close() noexcept;
        [[nodiscard]] Result<bool> Drain();

    private:
        [[nodiscard]] Result<std::shared_ptr<IResidentVfxGpuSimulation>> PrepareCaptured(const NativeVfxGpuSimulationPreparation &,
                                                                                         const std::shared_ptr<VulkanVfxGpuKernel> &,
                                                                                         const NativeVfxGpuPreparationResources &);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
