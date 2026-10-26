#pragma once

/** @file VulkanPresentationRuntime.h
 * @brief Device-private nonblocking WSI acquisition and presentation debt owner. */
#include <VulkanBackendModule.h>
#include <functional>

namespace Horo::Render::Detail {
    /** @brief Same-instance/device parents and borrowed canonical command completion observer. */
    struct VulkanPresentationInitialization {
        VkInstance instance{};
        VkPhysicalDevice physical{};
        VkDevice device{};
        VkSurfaceKHR surface{};
        VkQueue presentQueue{};
        std::uint32_t graphicsFamily{};
        std::uint32_t presentFamily{};
        std::uint32_t frames{};
        PFN_vkGetInstanceProcAddr instanceResolver{};
        PFN_vkGetDeviceProcAddr deviceResolver{};
        const std::function<Result<bool>(RenderTimelinePoint)> &observeCompletion;
    };

    /** @brief Borrowed exact acquired output; no object in this view may outlive its frame. */
    struct VulkanPresentationFrame {
        VkImage image{};
        VkImageView view{};
        VkFormat format{};
        VkSemaphore renderFinished{};
        FramebufferExtent extent;
    };

    /** @brief Bounded owner-thread WSI state; native parents survive explicit terminal drain. */
    class VulkanPresentationRuntime final {
    public:
        VulkanPresentationRuntime();
        ~VulkanPresentationRuntime();
        VulkanPresentationRuntime(const VulkanPresentationRuntime &) = delete;
        VulkanPresentationRuntime &operator=(const VulkanPresentationRuntime &) = delete;
        [[nodiscard]] Result<void> Initialize(const VulkanPresentationInitialization &request);
        /** @brief Drains old generation and realizes the explicit frozen candidate, returning DrawableUnavailable while pending. */
        [[nodiscard]] Result<void> Resize(FramebufferExtent extent, PresentMode mode);
        /** @brief Borrows the realized surface format for the matching GUI pipeline; unavailable before realization. */
        [[nodiscard]] Result<VkFormat> OutputFormat() const;
        /** @brief Starts or polls one timeout-zero fence acquisition, never consuming an unsignaled fence. */
        [[nodiscard]] Result<bool> PollAvailability(FramebufferExtent extent);
        /** @brief Binds a signaled acquired image to the existing renderer frame identity. */
        [[nodiscard]] Result<VulkanPresentationFrame> BeginFrame(FrameToken frame);
        /** @brief Records the exact canonical tail after successful native command submission. */
        [[nodiscard]] Result<void> Submitted(FrameToken frame, RenderTimelinePoint completion);
        /** @brief Queues presentation of the submitted tail; keeps independent present-fence debt. */
        [[nodiscard]] Result<void> Present(FrameToken frame);
        /** @brief Marks unsent or accepted work for nonblocking return after all required proofs. */
        void Abort(FrameToken frame) noexcept;
        [[nodiscard]] Result<void> BeginShutdown();
        [[nodiscard]] Result<RenderBackendDrainProgress> PollDrain();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
