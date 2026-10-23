#include "VulkanBackendInternal.h"
#include "VulkanNativeRuntime.h"
#include "VulkanRenderBackendErrors.h"

#include <exception>

namespace Horo::Render {
    /** @copydoc IVulkanCommandPort::PollPresentationAvailability */
    Result<bool> IVulkanCommandPort::PollPresentationAvailability(FramebufferExtent) {
        return Result<bool>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
    }

    /** @copydoc IVulkanCommandPort::ResizePresentation */
    Result<void> IVulkanCommandPort::ResizePresentation(FramebufferExtent, PresentMode) {
        return Result<void>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
    }

    /** @copydoc IVulkanLoaderPort::CreatePresentationSurface */
    Result<VkSurfaceKHR> IVulkanLoaderPort::CreatePresentationSurface(VkInstance) {
        return Result<VkSurfaceKHR>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
    }

    /** @copydoc IVulkanLoaderPort::DestroyPresentationSurface */
    void IVulkanLoaderPort::DestroyPresentationSurface(VkInstance, VkSurfaceKHR surface) noexcept {
        if (surface != VK_NULL_HANDLE)
            std::terminate();
    }

    /** @copydoc GetVulkanRenderBackendModuleInfo */
    const RenderBackendModuleInfo &GetVulkanRenderBackendModuleInfo() noexcept {
        static const RenderBackendModuleInfo info{
            .id = RenderBackendId{"vulkan"},
            .displayName = "Vulkan",
            .windowRequirements = {.presentation = RenderPresentationKind::Vulkan, .resizable = true, .highPixelDensity = true},
            .supportsInteractivePresentation = false,
        };
        return info;
    }

    /** @copydoc RegisterVulkanRenderBackend */
    Result<void> RegisterVulkanRenderBackend(RenderBackendRegistry &registry, IVulkanLoaderPort &loaderPort,
                                             Detail::VulkanEditorGraphicsBridge *editorBridge) {
        auto runtime = Detail::CreateVulkanNativeRuntime(loaderPort, editorBridge);
        if (!runtime)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::PresentationInUse));
        return Detail::RegisterVulkanRenderBackendWithOwnedRuntimePort(registry, std::move(runtime));
    }

    /** @copydoc CreateVulkanAdapterDiscovery */
    std::unique_ptr<IRenderAdapterDiscovery> CreateVulkanAdapterDiscovery(IVulkanLoaderPort &loaderPort) {
        return Detail::CreateVulkanAdapterDiscoveryWithOwnedRuntimePort(Detail::CreateVulkanNativeRuntime(loaderPort));
    }
}  // namespace Horo::Render
