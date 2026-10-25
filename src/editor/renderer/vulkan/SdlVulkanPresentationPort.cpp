#include "SdlVulkanPresentationPort.h"

#include "VulkanRenderBackendErrors.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <bit>
#include <exception>

namespace Horo::Editor {
    namespace {
        /** @brief Retains SDL's diagnostic at the owning native boundary. */
        Error SdlFailure(const char *operation) {
            return MakeError(Render::VulkanBackendErrors::NativeCallFailed, std::string{operation} + ": " + SDL_GetError());
        }
    }  // namespace

    /** @copydoc SdlVulkanPresentationPort::SdlVulkanPresentationPort */
    SdlVulkanPresentationPort::SdlVulkanPresentationPort(SDL_Window &window) noexcept : window_(&window) {}

    /** @copydoc SdlVulkanPresentationPort::~SdlVulkanPresentationPort */
    SdlVulkanPresentationPort::~SdlVulkanPresentationPort() {
        ReleaseLoader();
    }

    /** @copydoc SdlVulkanPresentationPort::CheckOwner */
    Result<void> SdlVulkanPresentationPort::CheckOwner() const {
        return owner_ == std::this_thread::get_id() ? Result<void>::Success()
                                                    : Result<void>::Failure(MakeError(Render::VulkanBackendErrors::WrongThread));
    }

    /** @copydoc SdlVulkanPresentationPort::AcquireLoader */
    Result<void> SdlVulkanPresentationPort::AcquireLoader() {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (loaded_)
            return Result<void>::Failure(MakeError(Render::VulkanBackendErrors::PresentationInUse));
        if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_VULKAN) == 0)
            return Result<void>::Failure(MakeError(Render::VulkanBackendErrors::PresentationUnsupported));
        if (!SDL_Vulkan_LoadLibrary(nullptr))
            return Result<void>::Failure(SdlFailure("SDL_Vulkan_LoadLibrary"));
        resolver_ = std::bit_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (resolver_ == nullptr) {
            SDL_Vulkan_UnloadLibrary();
            return Result<void>::Failure(SdlFailure("SDL_Vulkan_GetVkGetInstanceProcAddr"));
        }
        loaded_ = true;
        return Result<void>::Success();
    }

    /** @copydoc SdlVulkanPresentationPort::GetInstanceProcAddr */
    PFN_vkGetInstanceProcAddr SdlVulkanPresentationPort::GetInstanceProcAddr() noexcept {
        return owner_ == std::this_thread::get_id() && loaded_ ? resolver_ : nullptr;
    }

    /** @copydoc SdlVulkanPresentationPort::RequiredPresentationExtensions */
    Result<std::vector<std::string>> SdlVulkanPresentationPort::RequiredPresentationExtensions() {
        if (auto owner = CheckOwner(); owner.HasError())
            return Result<std::vector<std::string>>::Failure(owner.ErrorValue());
        if (!loaded_)
            return Result<std::vector<std::string>>::Failure(MakeError(Render::VulkanBackendErrors::NotInitialized));
        Uint32 count{};
        const auto names = SDL_Vulkan_GetInstanceExtensions(&count);
        if (names == nullptr || count == 0 || count > 64)
            return Result<std::vector<std::string>>::Failure(SdlFailure("SDL_Vulkan_GetInstanceExtensions"));
        std::vector<std::string> extensions;
        extensions.reserve(count);
        for (Uint32 index = 0; index < count; ++index)
            extensions.emplace_back(names[index]);
        extensions.emplace_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        extensions.emplace_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        return Result<std::vector<std::string>>::Success(std::move(extensions));
    }

    /** @copydoc SdlVulkanPresentationPort::CreatePresentationSurface */
    Result<VkSurfaceKHR> SdlVulkanPresentationPort::CreatePresentationSurface(const VkInstance instance) {
        if (auto owner = CheckOwner(); owner.HasError())
            return Result<VkSurfaceKHR>::Failure(owner.ErrorValue());
        if (!loaded_ || instance == VK_NULL_HANDLE || surface_ != VK_NULL_HANDLE)
            return Result<VkSurfaceKHR>::Failure(MakeError(Render::VulkanBackendErrors::PresentationInUse));
        VkSurfaceKHR surface{};
        if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface))
            return Result<VkSurfaceKHR>::Failure(SdlFailure("SDL_Vulkan_CreateSurface"));
        instance_ = instance;
        surface_ = surface;
        return Result<VkSurfaceKHR>::Success(surface_);
    }

    /** @copydoc SdlVulkanPresentationPort::DestroyPresentationSurface */
    void SdlVulkanPresentationPort::DestroyPresentationSurface(const VkInstance instance, const VkSurfaceKHR surface) noexcept {
        if (surface == VK_NULL_HANDLE)
            return;
        if (owner_ != std::this_thread::get_id() || instance != instance_ || surface != surface_)
            std::terminate();
        SDL_Vulkan_DestroySurface(instance, surface, nullptr);
        surface_ = VK_NULL_HANDLE;
        instance_ = VK_NULL_HANDLE;
    }

    /** @copydoc SdlVulkanPresentationPort::SupportsPresentation */
    Result<bool> SdlVulkanPresentationPort::SupportsPresentation(const VkPhysicalDevice device, const std::uint32_t queueFamily) {
        if (auto owner = CheckOwner(); owner.HasError())
            return Result<bool>::Failure(owner.ErrorValue());
        if (surface_ == VK_NULL_HANDLE || resolver_ == nullptr)
            return Result<bool>::Failure(MakeError(Render::VulkanBackendErrors::NotInitialized));
        const auto support =
            std::bit_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(resolver_(instance_, "vkGetPhysicalDeviceSurfaceSupportKHR"));
        if (support == nullptr)
            return Result<bool>::Failure(MakeError(Render::VulkanBackendErrors::EntryPointMissing));
        VkBool32 supported{};
        if (const auto result = support(device, queueFamily, surface_, &supported); result != VK_SUCCESS)
            return Result<bool>::Failure(MakeError(Render::VulkanBackendErrors::NativeCallFailed,
                                                   "vkGetPhysicalDeviceSurfaceSupportKHR: " + std::to_string(result)));
        return Result<bool>::Success(supported == VK_TRUE);
    }

    /** @copydoc SdlVulkanPresentationPort::IsDriverAllowed */
    Result<bool> SdlVulkanPresentationPort::IsDriverAllowed(const VkPhysicalDeviceProperties &) {
        if (auto owner = CheckOwner(); owner.HasError())
            return Result<bool>::Failure(owner.ErrorValue());
        return Result<bool>::Success(true);
    }

    /** @copydoc SdlVulkanPresentationPort::ReleaseLoader */
    void SdlVulkanPresentationPort::ReleaseLoader() noexcept {
        if (!loaded_)
            return;
        if (owner_ != std::this_thread::get_id() || surface_ != VK_NULL_HANDLE)
            std::terminate();
        SDL_Vulkan_UnloadLibrary();
        loaded_ = false;
        resolver_ = nullptr;
    }
}  // namespace Horo::Editor
