#pragma once

/** @file VulkanGraphRuntime.h
 * @brief Device-private translation of canonical graph submissions and resource states.
 */
#include "VulkanResourceRuntime.h"
#include "VulkanUiPaintCommands.h"

#include <VulkanEditorGraphicsBridge.h>

namespace Horo::Render::Detail {
    /** @brief Acquired WSI output, borrowed until this same frame submits or aborts. */
    struct VulkanGuiOutput {
        VkImage image{};
        VkImageView view{};
        VkSemaphore renderFinished{};
        FramebufferExtent extent;
        ClearColor clearColor;
        bool legacyUnmanagedOutput{false}; /**< Actual admitted primary-output policy, never inferred from a native format alone. */
    };

    /** @brief Borrowed selected-device configuration consumed synchronously during graph initialization. */
    struct VulkanGraphInitialization {
        VkDevice device;
        std::span<const VulkanCommandQueue> queues;
        std::uint32_t frames;
        std::uint64_t timelineDifference;
        std::uint64_t deviceIncarnation;
        VulkanResourceRuntime &resources;
        const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver;
        bool vertexLayerEnabled{false};
        const VkPhysicalDeviceProperties
            *properties{}; /**< Actual selected device facts; absent old fake ports expose no particle execution. */
    };

    /** @brief Bounded render-owner graph recorder; native parents outlive its explicit drain. */
    class VulkanGraphRuntime final {
    public:
        VulkanGraphRuntime();
        ~VulkanGraphRuntime();
        VulkanGraphRuntime(const VulkanGraphRuntime &) = delete;
        VulkanGraphRuntime &operator=(const VulkanGraphRuntime &) = delete;
        /** @brief Prepares fixed graph scratch/retention storage and required Vulkan1.3 dispatch.
         * @param request Selected device, exact queue topology, bounded frames, timeline limit, canonical identity,
         * borrowed same-device registry/resolver, and actual enabled vertex-layer feature.
         * @return Success or original typed rollback-safe failure. */
        [[nodiscard]] Result<void> Initialize(const VulkanGraphInitialization &request);
        /** @brief Returns effective indexed draw and explicitly enabled layer routing facts for this device. */
        [[nodiscard]] MultiviewCapabilities MultiviewSupport() const noexcept;
        [[nodiscard]] Result<void> BeginFrame(FrameToken frame);
        /** @brief Borrows the exact active frame's retired retention slot for exclusive editor ring reuse. */
        [[nodiscard]] Result<VulkanFrameRetentionSlot> FrameSlot(FrameToken frame) const;
        /** @brief Reports whether this current frame still precedes every native recording use. */
        [[nodiscard]] bool CanWriteFrameInputs(FrameToken frame) const noexcept;
        /** @copydoc IVulkanEditorGraphicsPort::ObservePreparationBuffer */
        [[nodiscard]] Result<bool> ObservePreparationBuffer(std::uint64_t instance);
        /** @copydoc IVulkanEditorGraphicsPort::SupportsStereoCoverageReadback */
        [[nodiscard]] bool SupportsStereoCoverageReadback() const noexcept;
        /** @copydoc IVulkanEditorGraphicsPort::ReadCompletedStereoCoverage */
        [[nodiscard]] Result<bool> ReadCompletedStereoCoverage(std::uint64_t instance, RenderTimelinePoint completion,
                                                               std::span<std::byte> destination);
        /** @copydoc IVulkanEditorGraphicsPort::SupportsStereoCoverageReduction */
        [[nodiscard]] bool SupportsStereoCoverageReduction(RenderQueueId queue) const noexcept;
        /** @copydoc IVulkanEditorGraphicsPort::PrepareStereoCoverageReduction */
        [[nodiscard]] Result<std::shared_ptr<IResidentStereoCoverageReducer>> PrepareStereoCoverageReduction(
            const VulkanStereoCoveragePreparation &request);
        /** @copydoc IVulkanEditorGraphicsPort::ReadCompletedStereoCoverageReduction */
        [[nodiscard]] Result<bool> ReadCompletedStereoCoverageReduction(std::uint64_t instance, RenderTimelinePoint completion,
                                                                        RenderGraphStereoCoverageReduction::Tallies &destination);
        /** @copydoc IVulkanEditorGraphicsPort::WritePreparationUniform */
        [[nodiscard]] Result<void> WritePreparationUniform(std::uint64_t instance, std::span<const std::byte> bytes);
        /** @copydoc IVulkanEditorGraphicsPort::WriteFrameBuffer */
        [[nodiscard]] Result<void> WriteFrameBuffer(FrameToken frame, std::uint64_t instance, std::span<const std::byte> bytes);
        [[nodiscard]] Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request);
        /** @brief Records one bounded reflected material pass using this runtime's existing submission owner.
         * @param request Exact frame, offscreen target, immutable draws and explicit frontend submission/lease.
         * @return Success with synchronous lease transfer or typed validation/native recording failure. */
        [[nodiscard]] Result<void> ExecuteMaterialPass(const RenderMaterialExecutionRequest &request);
        /** @brief Publishes an issued VFX block outside active recording, polling the sole native completion owner. */
        [[nodiscard]] Result<void> UpdateVfxFrame(const NativeVfxFrameUpdate &update);
        /** @brief Publishes a complete UI slot after polling all three native residents outside active frames. */
        [[nodiscard]] Result<void> UpdateUiPaintFrame(const NativeUiPaintFrameUpdate &update);
        /** @brief Cold-realizes a UI kernel with exact selected physical resource evidence and native renderer dispatch. */
        [[nodiscard]] Result<std::shared_ptr<IResidentUiPaintKernel>> RealizeUiPaintKernel(
            const CookedUiPaintKernel &cooked, const VulkanUiPaintDevice &device, const VulkanUiPaintCommands::FormatFeatures &formats,
            const VulkanShaderRuntime::Resolver &resolve);
        /** @brief Admits the existing GUI completion lease before any executor records native commands. */
        [[nodiscard]] Result<void> PrepareGuiComposition(const GuiCompositionRequest &request);
        /** @brief Records one GUI tail through this frame's sole submission/retention owner. */
        [[nodiscard]] Result<void> ExecuteGui(const GuiCompositionRequest &request, const VulkanGuiOutput &output,
                                              IVulkanGuiEncoder &encoder);
        [[nodiscard]] Result<NativeVfxGpuPreparationResources> QueryVfxGpuNativePreparationResources(
            const VfxGpuSimulationDescriptor &descriptor) const;
        [[nodiscard]] Result<std::shared_ptr<IResidentVfxGpuKernel>> RealizeVfxGpuPrograms(const CookedVfxGpuPrograms &programs);
        [[nodiscard]] Result<std::shared_ptr<IResidentVfxGpuSimulation>> PrepareNativeVfxGpuSimulation(
            const NativeVfxGpuSimulationPreparation &preparation);
        [[nodiscard]] Result<std::size_t> PollNativeVfxGpuReceipts(const std::shared_ptr<IResidentVfxGpuSimulation> &simulation,
                                                                   std::span<NativeGpuVfxStepReceipt> destination);
        [[nodiscard]] Result<std::size_t> CopyCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &simulation,
                                                                         const VfxGpuStepIdentity &step, std::span<std::byte> destination);
        [[nodiscard]] Result<void> DiscardCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &simulation,
                                                                     const VfxGpuStepIdentity &step);
        /** @brief Realizes one offline-cooked reconstruction kernel before beginning a frame. */
        [[nodiscard]] Result<std::shared_ptr<IResidentNativeReconstructionKernel>> RealizeNativeReconstructionKernel(
            const CookedNativeReconstructionKernel &kernel);
        /** @brief Reports exact native dispatch and selected compute-queue support. */
        [[nodiscard]] bool SupportsNativeReconstruction() const noexcept;
        /** @brief Reports available graph-owned material raster recording on this initialized device. */
        [[nodiscard]] bool SupportsGraphMaterialRaster() const noexcept;
        [[nodiscard]] bool SupportsVfxGpu() const noexcept;
        /** @brief Reports the initialized indexed forward draw route without requiring GPU particle compute.
         * @return True only for the live resource owner and all required native raster entry points.
         */
        [[nodiscard]] bool SupportsVfxForwardInstancing() const noexcept;
        /** @brief Submits exact copied Horo records; partial success retains accepted work and closes admission on failure. */
        [[nodiscard]] Result<void> SubmitFrame(FrameToken frame);
        void AbortFrame(FrameToken frame) noexcept;
        /** @copydoc IVulkanEditorGraphicsPort::WasSubmissionAccepted */
        [[nodiscard]] Result<bool> WasSubmissionAccepted(RenderTimelinePoint signal, std::uint64_t deviceIncarnation) const;
        [[nodiscard]] Result<bool> ObserveSubmission(const RenderCpuWait &wait);
        [[nodiscard]] Result<void> BeginShutdown();
        [[nodiscard]] Result<RenderBackendDrainProgress> PollDrain();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
