#pragma once

/** @file OpenXRVulkanSwapchain.h
 * @brief Non-installed concrete OpenXR/Vulkan runtime-image translation into Renderer.
 */

#include "OpenXRSwapchain.h"
#include "VulkanExternalImages.h"

#include <openxr/openxr_platform.h>

namespace Horo::XR::OpenXRInternal {
    /**
     * @brief Concrete official Vulkan image enumeration and canonical frontend import/retirement adapter.
     * @details The graphics binding is the exact stable storage used to create the native session,
     * not a second selected device. Host graphics/device/session/owner/frontend outlive Reset and
     * successful swapchain Close. Before PollRelease, the host polls its canonical frontend submission
     * points; this adapter observes retirement and never asserts completion from submitted values.
     * Optional motion/density/foveation paths remain unavailable until their full qualified contract
     * exists. Color/depth import is bounded N-layer, single-sample, without native allocation adoption.
     */
    class OpenXRVulkanSwapchainImages final : public IOpenXRSwapchainImages {
    public:
        /** @brief Borrows the exact composed frontend, session graphics port/binding and Horo graphics queue. */
        OpenXRVulkanSwapchainImages(Render::RenderFrontend &frontend, IOpenXRGraphicsBinding &graphics,
                                    const XrGraphicsBindingVulkanKHR &binding, Render::RenderQueueId queue);
        ~OpenXRVulkanSwapchainImages() override;
        OpenXRVulkanSwapchainImages(const OpenXRVulkanSwapchainImages &) = delete;
        OpenXRVulkanSwapchainImages &operator=(const OpenXRVulkanSwapchainImages &) = delete;
        /** @brief Binds once before native creation; owner and translator share the same control thread. */
        [[nodiscard]] Result<void> Bind(OpenXRSwapchain &owner);
        [[nodiscard]] Result<void> Validate(const NativeSwapchainRequest &request) const override;
        [[nodiscard]] Result<std::int64_t> SelectFormat(const NativeSwapchainRequest &request,
                                                        std::span<const std::int64_t> formats) const override;
        [[nodiscard]] Result<void> Enumerate(std::uint32_t count, const IOpenXRSwapchainImageEnumeration &enumeration) override;
        [[nodiscard]] Result<Render::RenderTextureHandle> Import(const NativeSwapchainRequest &request, const XRSwapchainImageId &image,
                                                                 std::uint32_t index) override;
        [[nodiscard]] Result<void> Retire(Render::RenderTextureHandle texture) override;
        [[nodiscard]] Result<bool> RetirementComplete(Render::RenderTextureHandle texture) override;
        [[nodiscard]] Result<void> Reset() override;

    private:
        struct Source;
        Render::RenderFrontend *frontend_;
        IOpenXRGraphicsBinding *graphics_;
        const XrGraphicsBindingVulkanKHR *binding_;
        Render::RenderQueueId queue_;
        OpenXRSwapchain *owner_{};
        std::array<std::shared_ptr<Source>, 64> sources_;
        std::uint32_t count_{};
        Render::RenderTextureHandle imported_;
        bool retirementConfirmed_{};
        std::uint32_t importedIndex_{};
    };
}  // namespace Horo::XR::OpenXRInternal
