#pragma once
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "VulkanRenderBackendErrors.h"
#include "VulkanVfxGpuRuntime.h"

#include <array>
#include <limits>
#include <thread>

namespace Horo::Render::Detail {
    struct VulkanVfxGpuDispatch final {
        PFN_vkCreateShaderModule createModule{};
        PFN_vkDestroyShaderModule destroyModule{};
        PFN_vkCreateComputePipelines createCompute{};
        PFN_vkCreateGraphicsPipelines createGraphics{};
        PFN_vkDestroyPipeline destroyPipeline{};
        PFN_vkCreateDescriptorSetLayout createSetLayout{};
        PFN_vkDestroyDescriptorSetLayout destroySetLayout{};
        PFN_vkCreatePipelineLayout createLayout{};
        PFN_vkDestroyPipelineLayout destroyLayout{};
        PFN_vkCreateDescriptorPool createPool{};
        PFN_vkDestroyDescriptorPool destroyPool{};
        PFN_vkAllocateDescriptorSets allocateSets{};
        PFN_vkUpdateDescriptorSets updateSets{};
        PFN_vkCmdBindPipeline bindPipeline{};
        PFN_vkCmdBindDescriptorSets bindSets{};
        PFN_vkCmdDispatch dispatch{};
        PFN_vkCmdDraw draw{};
        PFN_vkCmdBeginRendering beginRendering{};
        PFN_vkCmdEndRendering endRendering{};
        PFN_vkCmdSetViewport viewport{};
        PFN_vkCmdSetScissor scissor{};
        PFN_vkCmdPipelineBarrier2 barrier{};
    };

    struct VulkanVfxGpuDevice final {
        VkDevice device{};
        VkPhysicalDeviceProperties properties{};
        VulkanVfxGpuDispatch native;
        std::uint64_t incarnation{};
        std::uint32_t frames{};
        bool open{};
    };

    class VulkanVfxGpuKernel final : public IResidentVfxGpuKernel {
    public:
        ~VulkanVfxGpuKernel() override;
        void Reset() noexcept;
        std::shared_ptr<VulkanVfxGpuDevice> owner;
        VkPipeline compute{};
        VkPipeline drawColor{};
        VkPipeline drawDepth{};
        VkPipelineLayout computeLayout{};
        VkPipelineLayout drawLayout{};
        VkDescriptorSetLayout computeSet{};
        VkDescriptorSetLayout drawSet{};
        std::array<std::uint32_t, 5> computeBindings{};
        std::array<std::uint32_t, 2> drawBindings{};
    };

    struct VulkanVfxGpuBuffer final {
        VulkanVfxGpuBufferCapture capture;
        std::uint64_t generation{};
        VfxGpuStepIdentity observationStep;
        bool held{};
        bool completed{};
        bool failed{};
    };

    struct VulkanVfxGpuOperation final {
        VulkanCommandTicket ticket;
        RenderTimelinePoint point;
        std::size_t journal{std::numeric_limits<std::size_t>::max()};
        bool occupied{};
        bool accepted{};
        bool uncertain{};
    };

    struct VulkanVfxGpuJournal final {
        VfxGpuStepIdentity step;
        RenderTimelinePoint point;
        VulkanCommandTicket ticket;
        std::size_t source{};
        std::size_t destination{};
        std::size_t observation{};
        std::size_t operation{};
        bool occupied{};
        bool accepted{};
        bool observe{};
        bool uncertain{};
    };

    class VulkanVfxGpuSimulation final : public IResidentVfxGpuSimulation {
    public:
        ~VulkanVfxGpuSimulation() override;
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept override;
        void ReleaseNative() noexcept;
        [[nodiscard]] bool Queue(NativeGpuVfxStepReceipt) noexcept;
        std::shared_ptr<VulkanVfxGpuDevice> owner;
        std::shared_ptr<VulkanVfxGpuKernel> kernel;
        std::shared_ptr<IRenderGraphResourceLease> lease;
        VfxGpuSimulationId id;
        VfxGpuSimulationDescriptor descriptor;
        NativeVfxGpuPreparationResources plan;
        VulkanVfxGpuBufferCapture arena;
        std::vector<VulkanVfxGpuBuffer> states;
        std::vector<VulkanVfxGpuBuffer> observations;
        VulkanVfxGpuBuffer forces;
        VkDescriptorPool pool{};
        std::vector<VkDescriptorSet> computeSets;
        std::vector<VkDescriptorSet> drawSets;
        std::vector<VulkanVfxGpuOperation> operations;
        std::vector<std::size_t> active;
        std::size_t operationCursor{};
        std::vector<VulkanVfxGpuJournal> journals;
        std::vector<NativeGpuVfxStepReceipt> receipts;
        std::size_t head{};
        std::size_t count{};
        std::size_t reserved{};
        std::size_t source{};
        std::uint64_t generation{1};
        std::uint64_t tick{};
        bool stopped{};
    };

    struct VulkanVfxGpuRuntime::Impl {
        std::shared_ptr<VulkanVfxGpuDevice> device;
        VulkanResourceRuntime *resources{};
        VulkanCommandRuntime *commands{};
        std::thread::id owner;
        std::vector<std::shared_ptr<VulkanVfxGpuKernel>> kernels;
        std::vector<std::shared_ptr<VulkanVfxGpuSimulation>> simulations;
        bool closing{};
        std::size_t retirementCursor{};
        [[nodiscard]] Result<void> Retire(std::size_t budget);
        [[nodiscard]] Result<void> Access(bool admission = true) const;
        [[nodiscard]] Result<void> ValidateColdInventory(const VulkanVfxGpuSimulation &) const;
        [[nodiscard]] Result<std::shared_ptr<VulkanVfxGpuSimulation>> Simulation(const std::shared_ptr<IResidentVfxGpuSimulation> &) const;
        [[nodiscard]] Result<bool> Complete(RenderTimelinePoint, bool refresh = true) const;
        [[nodiscard]] Result<void> Observe(VulkanVfxGpuSimulation &);
        [[nodiscard]] Result<void> ValidateCompute(const RenderGraphVfxGpuCompute &, const RenderGraphExecutionRequest &) const;
        [[nodiscard]] Result<void> ValidateDraw(const RenderGraphVfxGpuDraw &, const RenderGraphExecutionRequest &,
                                                std::span<const VulkanGraphResource>) const;
        [[nodiscard]] Result<void> EncodeCompute(VkCommandBuffer, VulkanCommandTicket, const RenderQueueSubmission &,
                                                 const RenderGraphVfxGpuCompute &, const RenderGraphExecutionRequest &);
        [[nodiscard]] Result<void> EncodeDraw(VkCommandBuffer, VulkanCommandTicket, const RenderQueueSubmission &,
                                              const RenderGraphVfxGpuDraw &, const RenderGraphExecutionRequest &,
                                              std::span<const VulkanGraphResource>);
        void Finish(VulkanVfxGpuSimulation &, VulkanCommandTicket, bool) noexcept;
        void Reject(VulkanVfxGpuSimulation &, const RenderGraphVfxGpuCompute &) noexcept;
    };

    [[nodiscard]] Error VulkanVfxGpuInvalid(const char *);
    [[nodiscard]] Error VulkanVfxGpuNativeFailure(const char *, VkResult);
    [[nodiscard]] std::uint64_t VulkanVfxGpuInstance(RenderGraphResourceId, const RenderGraphExecutionRequest &) noexcept;
    [[nodiscard]] std::size_t VulkanVfxGpuState(const VulkanVfxGpuSimulation &, RenderGraphResourceId,
                                                const RenderGraphExecutionRequest &) noexcept;
    [[nodiscard]] std::size_t VulkanVfxGpuObservation(const VulkanVfxGpuSimulation &, RenderGraphResourceId,
                                                      const RenderGraphExecutionRequest &) noexcept;
    [[nodiscard]] Result<void> PrepareVulkanVfxGpuDescriptors(VulkanVfxGpuSimulation &);
    [[nodiscard]] std::size_t ReserveVulkanVfxGpuOperation(VulkanVfxGpuSimulation &, VulkanCommandTicket, RenderTimelinePoint) noexcept;
    void WriteVulkanVfxGpuParameters(VulkanVfxGpuSimulation &, std::size_t, std::span<const std::uint32_t>) noexcept;
    [[nodiscard]] bool ContainsVulkanVfxGpuBuffer(const VulkanVfxGpuSimulation &, VkBuffer, bool arena = false) noexcept;
}  // namespace Horo::Render::Detail
