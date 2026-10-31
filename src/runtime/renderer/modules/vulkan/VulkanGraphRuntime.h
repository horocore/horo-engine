#pragma once

/** @file VulkanGraphRuntime.h
 * @brief Device-private translation of canonical graph submissions and resource states.
 */
#include "VulkanResourceRuntime.h"

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
        [[nodiscard]] Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request);
        /** @brief Records one bounded reflected material pass using this runtime's existing submission owner.
         * @param request Exact frame, offscreen target, immutable draws and explicit frontend submission/lease.
         * @return Success with synchronous lease transfer or typed validation/native recording failure. */
        [[nodiscard]] Result<void> ExecuteMaterialPass(const RenderMaterialExecutionRequest &request);
        /** @brief Publishes an issued VFX block outside active recording, polling the sole native completion owner. */
        [[nodiscard]] Result<void> UpdateVfxFrame(const NativeVfxFrameUpdate &update);
        /** @brief Admits the existing GUI completion lease before any executor records native commands. */
        [[nodiscard]] Result<void> PrepareGuiComposition(const GuiCompositionRequest &request);
        /** @brief Records one GUI tail through this frame's sole submission/retention owner. */
        [[nodiscard]] Result<void> ExecuteGui(const GuiCompositionRequest &request, const VulkanGuiOutput &output,
                                              IVulkanGuiEncoder &encoder);
        /** @brief Submits exact copied Horo records; partial success retains accepted work and closes admission on failure. */
        [[nodiscard]] Result<void> SubmitFrame(FrameToken frame);
        void AbortFrame(FrameToken frame) noexcept;
        [[nodiscard]] Result<bool> ObserveSubmission(const RenderCpuWait &wait);
        [[nodiscard]] Result<void> BeginShutdown();
        [[nodiscard]] Result<RenderBackendDrainProgress> PollDrain();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
