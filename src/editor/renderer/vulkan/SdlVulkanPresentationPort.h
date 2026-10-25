#pragma once

/** @file SdlVulkanPresentationPort.h
 * @brief SDL window/loader lease for the same-instance Vulkan presentation owner. */
#include "VulkanBackendModule.h"

#include <thread>

struct SDL_Window;

namespace Horo::Editor {
    /** @brief Borrows one Vulkan-capable SDL window; native runtime owns the surface lease. */
    class SdlVulkanPresentationPort final : public Render::IVulkanLoaderPort {
    public:
        /** @brief Captures owner thread; the window outlives the frontend, surfaces and this port. */
        explicit SdlVulkanPresentationPort(SDL_Window &window) noexcept;
        ~SdlVulkanPresentationPort() override;
        SdlVulkanPresentationPort(const SdlVulkanPresentationPort &) = delete;
        SdlVulkanPresentationPort &operator=(const SdlVulkanPresentationPort &) = delete;

        [[nodiscard]] Result<void> AcquireLoader() override;
        [[nodiscard]] PFN_vkGetInstanceProcAddr GetInstanceProcAddr() noexcept override;
        [[nodiscard]] Result<std::vector<std::string>> RequiredPresentationExtensions() override;
        [[nodiscard]] Result<VkSurfaceKHR> CreatePresentationSurface(VkInstance instance) override;
        void DestroyPresentationSurface(VkInstance instance, VkSurfaceKHR surface) noexcept override;
        [[nodiscard]] Result<bool> SupportsPresentation(VkPhysicalDevice device, std::uint32_t queueFamily) override;
        [[nodiscard]] Result<bool> IsDriverAllowed(const VkPhysicalDeviceProperties &properties) override;
        void ReleaseLoader() noexcept override;

    private:
        [[nodiscard]] Result<void> CheckOwner() const;
        const std::thread::id owner_{std::this_thread::get_id()};
        SDL_Window *window_{};
        bool loaded_{};
        PFN_vkGetInstanceProcAddr resolver_{};
        VkInstance instance_{};
        VkSurfaceKHR surface_{};
    };
}  // namespace Horo::Editor
