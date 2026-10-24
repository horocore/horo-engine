#pragma once
/** @file OpenXRSelectedVulkanBinding.h
 * @brief Non-installed same-instance XR requirements and selected Vulkan graphics binding.
 */
#include "OpenXRNativeSession.h"
#include "VulkanEditorGraphicsBridge.h"

#include <openxr/openxr_platform.h>

namespace Horo::XR::OpenXRInternal {
    /** @brief One startup-owned adapter; never creates a Vulkan instance/device or a second XR session.
     * @details Preflight runs after actual XR discovery and before window/device creation. The host attaches its
     * selected platform port once, then retains this adapter, the native session and platform through renderer drain.
     * All operations are serialized on the native session's control lane. Enabled requirements never change in place.
     */
    class OpenXRSelectedVulkanBinding final : public IOpenXRGraphicsBinding, public Render::IVulkanLoaderPort {
    public:
        /** @brief Queries exact same-instance native Vulkan requirements without publishing support.
         * @param owner Actual discovery borrow, used synchronously; retained handles remain owned by its session.
         * @return Complete immutable API/extension facts or original native failure. A failed query never becomes empty requirements.
         */
        [[nodiscard]] Result<void> Preflight(const NativeDiscoveryBorrow &owner);
        /** @brief Attaches the sole selected platform loader and backend graphics owner before creation.
         * @return Typed rejection for replacement, missing preflight or a different owner lane.
         */
        [[nodiscard]] Result<void> Attach(Render::IVulkanLoaderPort &platform, Render::Detail::VulkanEditorGraphicsBridge &graphics);
        [[nodiscard]] const char *RequiredExtension() const noexcept override;
        [[nodiscard]] Result<void> ValidatePreparation() const override;
        [[nodiscard]] Result<void> Validate(const XRSessionId &candidate) const override;
        [[nodiscard]] Result<const XrBaseInStructure *> Prepare(XrInstance instance, XrSystemId system,
                                                                PFN_xrGetInstanceProcAddr dispatch) override;
        void Release() noexcept override;
        /** @brief Borrows the exact stable binding used for this native session's creation.
         * @param candidate Exact activated Horo session, fenced against this selected device.
         * @return Stable binding address or original stale/device error; never another Vulkan instance/device.
         * @pre Owner lane. The borrow ends at Release/retirement; native image translators must drain first.
         */
        [[nodiscard]] Result<const XrGraphicsBindingVulkanKHR *> BorrowBinding(const XRSessionId &candidate) const;
        [[nodiscard]] Result<void> AcquireLoader() override;
        [[nodiscard]] PFN_vkGetInstanceProcAddr GetInstanceProcAddr() noexcept override;
        [[nodiscard]] Result<std::vector<std::string>> RequiredPresentationExtensions() override;
        [[nodiscard]] Result<Render::VulkanHostGraphicsRequirements> RequiredGraphicsRequirements() override;
        [[nodiscard]] Result<bool> AcceptPhysicalDevice(VkInstance instance, VkPhysicalDevice physical) override;
        [[nodiscard]] Result<VkSurfaceKHR> CreatePresentationSurface(VkInstance instance) override;
        void DestroyPresentationSurface(VkInstance instance, VkSurfaceKHR surface) noexcept override;
        [[nodiscard]] Result<bool> SupportsPresentation(VkPhysicalDevice device, std::uint32_t family) override;
        [[nodiscard]] Result<bool> IsDriverAllowed(const VkPhysicalDeviceProperties &properties) override;
        void ReleaseLoader() noexcept override;

    private:
        [[nodiscard]] Result<void> CheckLane() const;
        const std::thread::id lane_{std::this_thread::get_id()};
        XrInstance instance_{XR_NULL_HANDLE};
        XrSystemId system_{XR_NULL_SYSTEM_ID};
        PFN_xrGetInstanceProcAddr dispatch_{};
        PFN_xrGetVulkanGraphicsDeviceKHR selectDevice_{};
        std::optional<Render::VulkanHostGraphicsRequirements> requirements_;
        Render::IVulkanLoaderPort *platform_{};
        Render::Detail::VulkanEditorGraphicsBridge *graphics_{};
        XrGraphicsBindingVulkanKHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        bool acquired_{};
        bool bound_{};
    };
}  // namespace Horo::XR::OpenXRInternal
