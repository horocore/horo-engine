#include "VulkanBackendInternal.h"
#include "VulkanNativeRuntime.h"

namespace Horo::Render {
    /** @copydoc GetVulkanRenderBackendModuleInfo */
    const RenderBackendModuleInfo &GetVulkanRenderBackendModuleInfo() noexcept {
        static const RenderBackendModuleInfo info{
            .id = RenderBackendId{"vulkan"},
            .displayName = "Vulkan",
            .windowRequirements = {.presentation = RenderPresentationKind::Vulkan, .resizable = true, .highPixelDensity = true},
            .supportsInteractivePresentation = true,
        };
        return info;
    }

    /** @copydoc RegisterVulkanRenderBackend */
    Result<void> RegisterVulkanRenderBackend(RenderBackendRegistry &registry, IVulkanLoaderPort &loaderPort) {
        return Detail::RegisterVulkanRenderBackendWithOwnedRuntimePort(registry, Detail::CreateVulkanNativeRuntime(loaderPort));
    }

    /** @copydoc CreateVulkanAdapterDiscovery */
    std::unique_ptr<IRenderAdapterDiscovery> CreateVulkanAdapterDiscovery(IVulkanLoaderPort &loaderPort) {
        return Detail::CreateVulkanAdapterDiscoveryWithOwnedRuntimePort(Detail::CreateVulkanNativeRuntime(loaderPort));
    }
}  // namespace Horo::Render
