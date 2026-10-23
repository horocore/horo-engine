#pragma once
/** @file VulkanEditorGraphicsBridge.h
 * @brief Non-installed selected-device composition seam for the matching Vulkan editor pair. */
#include "VulkanBackendModule.h"

namespace Horo::Render::Detail {
    /** @brief Canonical retired retention slot, independent of swapchain image and encoder call counts. */
    struct VulkanFrameRetentionSlot {
        std::uint32_t index{};
        std::uint32_t count{};
    };

    /** @brief Borrowed exact device children held by the existing completion owner. */
    struct VulkanGuiSampleBinding {
        VkImageView view{};
        VkSampler sampler{};
    };

    /** @brief Borrowed coherent frontend-owned encoder buffer. */
    struct VulkanGuiBufferBinding {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize bytes{};
    };

    /** @brief Sole GUI native encoder: recording authority only, without submission or presentation. */
    class IVulkanGuiEncoder {
    public:
        virtual ~IVulkanGuiEncoder() = default;
        [[nodiscard]] virtual Result<void> Record(VkCommandBuffer command, VulkanFrameRetentionSlot slot,
                                                  std::span<const VulkanGuiSampleBinding> samples,
                                                  std::span<const VulkanGuiBufferBinding> buffers) = 0;
    };

    /** @brief Explicit host-owned device child released after canonical drain and before native device destruction. */
    class IVulkanEditorDeviceChild {
    public:
        virtual ~IVulkanEditorDeviceChild() = default;
        virtual void ReleaseNative() noexcept = 0;
    };

    /** @brief Borrowed same-instance/device facts, queried only after explicit surface realization. */
    struct VulkanEditorDevice {
        VkInstance instance{};
        VkPhysicalDevice physical{};
        VkDevice device{};
        VkQueue graphicsQueue{};
        std::uint32_t graphicsFamily{};
        std::uint32_t frames{};
        VkFormat outputFormat{};
        PFN_vkGetInstanceProcAddr instanceResolver{};
        PFN_vkGetDeviceProcAddr deviceResolver{};
        RenderQueueId graphicsIdentity;
    };

    /** @brief Explicit private composition port implemented by the selected runtime's existing owners. */
    class IVulkanEditorGraphicsPort {
    public:
        virtual ~IVulkanEditorGraphicsPort() = default;
        [[nodiscard]] virtual Result<VulkanEditorDevice> QueryDevice() const = 0;
        /** @brief Borrows exact frontend-owned sample children during load-time descriptor preparation. */
        [[nodiscard]] virtual Result<VulkanGuiSampleBinding> BorrowSample(const GuiSampledResourceInstance &sample) const = 0;
        [[nodiscard]] virtual Result<VulkanFrameRetentionSlot> FrameSlot(FrameToken frame) const = 0;
        [[nodiscard]] virtual Result<void> PreparePrimaryOutput(FrameToken frame, PrimaryOutputAttachment attachment) = 0;
        [[nodiscard]] virtual Result<void> ExecuteGui(const GuiCompositionRequest &request, IVulkanGuiEncoder &encoder) = 0;
        [[nodiscard]] virtual Result<void> WriteFrameBuffer(FrameToken frame, std::uint64_t instance, std::span<const std::byte> bytes) = 0;
        [[nodiscard]] virtual Result<void> AttachDeviceChild(IVulkanEditorDeviceChild &child) = 0;
        virtual void DetachDeviceChild(IVulkanEditorDeviceChild &child) noexcept = 0;
    };

    /** @brief Host-owned borrowed connection; runtime attachment never selects or creates another renderer. */
    class VulkanEditorGraphicsBridge final {
    public:
        [[nodiscard]] IVulkanEditorGraphicsPort *Port() const noexcept {
            return port_;
        }

        /** @brief Binds at most one selected runtime; the host keeps this object alive until runtime destruction. */
        [[nodiscard]] bool Attach(IVulkanEditorGraphicsPort &port) noexcept {
            if (port_ != nullptr)
                return false;
            port_ = &port;
            return true;
        }

        void Detach(IVulkanEditorGraphicsPort &port) noexcept {
            if (port_ == &port)
                port_ = nullptr;
        }

    private:
        IVulkanEditorGraphicsPort *port_{};
    };
}  // namespace Horo::Render::Detail
