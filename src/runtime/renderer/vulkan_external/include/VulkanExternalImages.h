#pragma once

/** @file VulkanExternalImages.h
 * @brief Non-installed Vulkan specialization of a qualified external acquisition proof.
 */

#include "RenderExternalImages.h"

#include <array>
#include <vulkan/vulkan.h>

namespace Horo::Render::Detail {
    /** @brief Cold-created views of the runtime-owned image; the source retains them through canonical retirement. */
    struct VulkanExternalNativeViews {
        VkImageView attachment{VK_NULL_HANDLE};
        static constexpr std::size_t MaximumLayers = 16;
        std::array<VkImageView, MaximumLayers> layers{}; /**< Exact single-layer attachment views, never image ownership. */
        std::uint32_t layerCount{}; /**< Complete cold-admitted prefix; partial preparation never publishes a source. */
    };

    /** @brief Exact selected device dispatch borrowed by the native swapchain owner, not another loader authority. */
    struct VulkanExternalViewDispatch {
        PFN_vkCreateImageView create{};
        PFN_vkDestroyImageView destroy{};
    };

    /**
     * @brief Native image borrowed from an external owner using this exact renderer VkDevice.
     * @details The concrete XR source is issued only after actual xrAcquireSwapchainImage and
     * xrWaitSwapchainImage. OpenXR's Vulkan contract gives the bound graphics queue ownership
     * in color/depth attachment layout and requires that same layout/queue on release. This
     * source never invents an external queue-family release or admits arbitrary foreign queues.
     */
    class IVulkanExternalTextureSource : public IRenderExternalTextureSource {
    public:
        /** @brief Returns the exact device already bound to the external runtime session. */
        [[nodiscard]] virtual VkDevice NativeDevice() const noexcept = 0;
        /** @brief Returns the runtime-owned image; Renderer must never destroy or free it. */
        [[nodiscard]] virtual VkImage NativeImage() const noexcept = 0;

        /** @brief Borrows optional cold native views; empty preserves non-XR external adapters' explicit creation path.
         * @details Actual XR sources require a nonempty attachment before publishing an acquisition. The first-eye
         * view exists only when sampled color usage was explicitly admitted before native swapchain creation.
         */
        [[nodiscard]] virtual VulkanExternalNativeViews PreparedNativeViews() const noexcept {
            return {};
        }

        /** @brief Admits only an explicitly prepared sampled first-eye mirror under the legacy unmanaged UNORM policy.
         * @param view Exact fresh renderer view descriptor; array/other-eye/sRGB/HDR requests remain unsupported.
         * @return Whether this native owner admits the exact view without changing its acquisition or final-state requirements.
         */
        [[nodiscard]] virtual bool AdmitsFirstEyeGuiMirror(const RenderTextureViewDescriptor &) const noexcept {
            return false;
        }

        /** @brief Returns the exact queue family bound to the runtime session; the native graph validates its logical mapping. */
        [[nodiscard]] virtual std::uint32_t NativeQueueFamily() const noexcept = 0;
        /** @brief Records actual committed layout/queue at native retirement, before the canonical lifetime releases.
         * @details Invoked only when the real native resource and queued references retire. It carries protocol state,
         * never a second completion timeline, counter or synthesized GPU-completion receipt. */
        virtual void RecordNativeRetirement(const RenderGraphLogicalState &state, bool hasNativeState) const noexcept = 0;
        /** @brief Returns actual native acquisition layout/queue evidence; contents may be undefined. */
        [[nodiscard]] RenderGraphLogicalState AcquiredState() const noexcept override = 0;
    };
}  // namespace Horo::Render::Detail
