#pragma once

/** @file VulkanExternalImages.h
 * @brief Non-installed Vulkan specialization of a qualified external acquisition proof.
 */

#include "RenderExternalImages.h"

#include <vulkan/vulkan.h>

namespace Horo::Render::Detail {
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
        /** @brief Returns the exact queue family bound to the runtime session; the native graph validates its logical mapping. */
        [[nodiscard]] virtual std::uint32_t NativeQueueFamily() const noexcept = 0;
        /** @brief Records actual committed layout/queue at native retirement, before the canonical lifetime releases.
         * @details Invoked only when the real native resource and queued references retire. It carries protocol state,
         * never a second completion timeline, counter or synthesized GPU-completion receipt. */
        virtual void RecordNativeRetirement(const RenderGraphLogicalState &state, bool hasNativeState) const noexcept = 0;
        /** @brief Returns actual native acquisition layout/queue evidence; contents may be undefined. */
        [[nodiscard]] virtual RenderGraphLogicalState AcquiredState() const noexcept = 0;
    };
}  // namespace Horo::Render::Detail
