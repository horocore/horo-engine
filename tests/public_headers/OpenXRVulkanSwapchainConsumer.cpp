#include "OpenXRVulkanSwapchain.h"

#include <type_traits>

static_assert(std::is_base_of_v<Horo::XR::OpenXRInternal::IOpenXRSwapchainImages, Horo::XR::OpenXRInternal::OpenXRVulkanSwapchainImages>);
static_assert(std::is_abstract_v<Horo::Render::Detail::IRenderExternalTextureBackend>);
static_assert(std::is_abstract_v<Horo::Render::Detail::IVulkanExternalTextureSource>);

// Only the deliberate non-installed bridge interfaces provide these headers/native types.
void RequireNativeBridgeBoundary(const XrGraphicsBindingVulkanKHR &binding) {
    (void)binding;
}
