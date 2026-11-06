#include "VulkanAndroidRuntime.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
#include <limits>
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace Horo::Render::AndroidVulkan {
    namespace {
        /** @brief Converts the loader's exact native procedure without assuming object/function pointer aliasing. */
        template <class T> T Function(PFN_vkGetInstanceProcAddr resolver, VkInstance instance, const char *name) {
            return std::bit_cast<T>(resolver(instance, name));
        }

        /** @brief Checks one bounded extension inventory for an exact required name. */
        bool Contains(std::span<const VkExtensionProperties> extensions, const char *name) {
            return std::ranges::any_of(extensions, [name](const auto &extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        }

        /** @brief Queries and admits the effective development profile's exact instance extensions. */
        Result<void> AdmitInstanceExtensions(PFN_vkEnumerateInstanceExtensionProperties enumerate) {
            if (!enumerate)
                return Result<void>::Failure(MakeError(Errors::Unsupported));
            std::array<VkExtensionProperties, 256> properties{};
            std::uint32_t count{};
            auto result = enumerate(nullptr, &count, nullptr);
            if (result != VK_SUCCESS)
                return Result<void>::Failure(NativeError("vkEnumerateInstanceExtensionProperties", result));
            if (count > properties.size())
                return Result<void>::Failure(MakeError(Errors::Capacity));
            result = enumerate(nullptr, &count, properties.data());
            if (result != VK_SUCCESS)
                return Result<void>::Failure(NativeError("vkEnumerateInstanceExtensionProperties", result));
            constexpr std::array names{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
                                       VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
            if (!std::ranges::all_of(names, [&](const char *name) {
                return Contains(std::span{properties}.first(count), name);
            }))
                return Result<void>::Failure(
                    MakeError(Errors::Unsupported, "Android development presentation requires surface maintenance instance dependencies."));
            return Result<void>::Success();
        }

        /** @brief Checks actual device extension and feature support without creating a window or surface. */
        Result<bool> SupportsMaintenance(PFN_vkGetInstanceProcAddr resolver, VkInstance instance, VkPhysicalDevice physical) {
            const auto enumerate =
                Function<PFN_vkEnumerateDeviceExtensionProperties>(resolver, instance, "vkEnumerateDeviceExtensionProperties");
            const auto query = Function<PFN_vkGetPhysicalDeviceFeatures2>(resolver, instance, "vkGetPhysicalDeviceFeatures2");
            if (!enumerate || !query)
                return Result<bool>::Failure(MakeError(Errors::Unsupported));
            std::array<VkExtensionProperties, 256> properties{};
            std::uint32_t count{};
            auto result = enumerate(physical, nullptr, &count, nullptr);
            if (result != VK_SUCCESS)
                return Result<bool>::Failure(NativeError("vkEnumerateDeviceExtensionProperties", result));
            if (count > properties.size())
                return Result<bool>::Failure(MakeError(Errors::Capacity));
            result = enumerate(physical, nullptr, &count, properties.data());
            if (result != VK_SUCCESS)
                return Result<bool>::Failure(NativeError("vkEnumerateDeviceExtensionProperties", result));
            const auto inventory = std::span{properties}.first(count);
            if (!Contains(inventory, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
                !Contains(inventory, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
                return Result<bool>::Success(false);
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &maintenance};
            query(physical, &features);
            return Result<bool>::Success(maintenance.swapchainMaintenance1 == VK_TRUE);
        }

        /** @brief Selects a bounded actual graphics queue inventory without claiming surface support. */
        std::optional<std::uint32_t> GraphicsFamily(PFN_vkGetInstanceProcAddr resolver, VkInstance instance, VkPhysicalDevice physical) {
            const auto query =
                Function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(resolver, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
            if (!query)
                return {};
            std::array<VkQueueFamilyProperties, 64> properties{};
            std::uint32_t count{};
            query(physical, &count, nullptr);
            if (count > properties.size())
                return {};
            query(physical, &count, properties.data());
            for (std::uint32_t index = 0; index < count; ++index)
                if (properties[index].queueCount && (properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                    return index;
            return {};
        }

        /** @brief Intersects actual standard API, maintenance and graphics-family facts while preserving native query failures. */
        Result<std::optional<std::uint32_t>> CandidateFamily(PFN_vkGetInstanceProcAddr resolver, VkInstance instance,
                                                             VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceProperties properties) {
            VkPhysicalDeviceProperties facts{};
            properties(physical, &facts);
            if (VK_API_VERSION_VARIANT(facts.apiVersion) != 0 || facts.apiVersion < VK_API_VERSION_1_1)
                return Result<std::optional<std::uint32_t>>::Success({});
            const auto supported = SupportsMaintenance(resolver, instance, physical);
            if (supported.HasError())
                return Result<std::optional<std::uint32_t>>::Failure(supported.ErrorValue());
            if (!supported.Value())
                return Result<std::optional<std::uint32_t>>::Success({});
            return Result<std::optional<std::uint32_t>>::Success(GraphicsFamily(resolver, instance, physical));
        }

        /** @brief Admits only the explicitly implemented single-queue interactive FIFO development configuration. */
        bool AdmitsConfiguration(const RenderBackendConfig &config) {
            const std::array supported{config.IsValid(),
                                       config.requirePresentation,
                                       config.HasSingleEffectiveQueue(),
                                       !config.adapter,
                                       !config.presentationOutput,
                                       !config.enableValidation,
                                       config.presentMode == PresentMode::Fifo};
            if (!std::ranges::all_of(supported, [](bool value) {
                return value;
            }))
                return false;
            const auto graphics = std::ranges::find(config.queueAssignments, RenderQueueRole::Graphics, &RenderQueueAssignment::role);
            const auto compute = std::ranges::find(config.queueAssignments, RenderQueueRole::Compute, &RenderQueueAssignment::role);
            return graphics != config.queueAssignments.end() && compute == config.queueAssignments.end();
        }
    }  // namespace

    Runtime::Runtime(Platform::Android::AndroidWindowHost &windows, PFN_vkGetInstanceProcAddr resolver) noexcept
        : windows_(windows), injected_(resolver != nullptr) {
        vk_.instanceProc = resolver;
    }

    Runtime::~Runtime() {
        if (device_) {
            if (Close().HasError())
                std::terminate();
            const auto drained = Drain();
            if (drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                std::terminate();
        }
        Rollback();
    }

    /** @brief Acquires only this backend's loader lease; protocol tests supply an explicit native function resolver. */
    Result<void> Runtime::OpenLoader() {
        if (injected_)
            return Result<void>::Success();
#if defined(__ANDROID__)
        library_ = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (library_)
            vk_.instanceProc = std::bit_cast<PFN_vkGetInstanceProcAddr>(dlsym(library_, "vkGetInstanceProcAddr"));
#endif
        if (!vk_.instanceProc)
            return Result<void>::Failure(MakeError(Errors::Unsupported, "The explicit Android Vulkan loader is unavailable."));
        return Result<void>::Success();
    }

    /** @brief Creates exactly one Vulkan 1.1 instance with explicitly admitted maintenance dependencies and no surface. */
    Result<void> Runtime::CreateInstance() {
        const auto admission = AdmitInstanceExtensions(
            Function<PFN_vkEnumerateInstanceExtensionProperties>(vk_.instanceProc, {}, "vkEnumerateInstanceExtensionProperties"));
        if (admission.HasError())
            return admission;
        const auto create = Function<PFN_vkCreateInstance>(vk_.instanceProc, {}, "vkCreateInstance");
        if (!create)
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        constexpr std::array extensions{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
                                        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
        const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                    .pApplicationName = "Horo Android development",
                                    .apiVersion = VK_API_VERSION_1_1};
        const VkInstanceCreateInfo info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                        .pApplicationInfo = &app,
                                        .enabledExtensionCount = extensions.size(),
                                        .ppEnabledExtensionNames = extensions.data()};
        const auto result = create(&info, nullptr, &instance_);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateInstance", result));
        vk_.destroyInstance = Function<PFN_vkDestroyInstance>(vk_.instanceProc, instance_, "vkDestroyInstance");
        if (!vk_.destroyInstance)
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        return Result<void>::Success();
    }

    /** @brief Admits an actual graphics device and maintenance feature before the host supplies any native window. */
    Result<void> Runtime::SelectDevice() {
        const auto enumerate = Function<PFN_vkEnumeratePhysicalDevices>(vk_.instanceProc, instance_, "vkEnumeratePhysicalDevices");
        const auto properties = Function<PFN_vkGetPhysicalDeviceProperties>(vk_.instanceProc, instance_, "vkGetPhysicalDeviceProperties");
        if (!enumerate || !properties)
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        std::array<VkPhysicalDevice, 32> devices{};
        std::uint32_t count{};
        auto result = enumerate(instance_, &count, nullptr);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkEnumeratePhysicalDevices", result));
        if (count > devices.size())
            return Result<void>::Failure(MakeError(Errors::Capacity));
        result = enumerate(instance_, &count, devices.data());
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkEnumeratePhysicalDevices", result));
        for (const auto device : std::span{devices}.first(count)) {
            const auto family = CandidateFamily(vk_.instanceProc, instance_, device, properties);
            if (family.HasError())
                return Result<void>::Failure(family.ErrorValue());
            if (family.Value())
                return CreateDevice(device, *family.Value());
        }
        return Result<void>::Failure(
            MakeError(Errors::Unsupported, "No actual Vulkan 1.1 graphics device admits the maintenance-enabled development profile."));
    }

    /** @brief Enables only the actually queried maintenance feature and creates one graphics/present queue. */
    Result<void> Runtime::CreateDevice(VkPhysicalDevice physical, std::uint32_t family) {
        const auto create = Function<PFN_vkCreateDevice>(vk_.instanceProc, instance_, "vkCreateDevice");
        vk_.deviceProc = Function<PFN_vkGetDeviceProcAddr>(vk_.instanceProc, instance_, "vkGetDeviceProcAddr");
        if (!create || !vk_.deviceProc)
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        constexpr float priority = 1;
        const VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                            .queueFamilyIndex = family,
                                            .queueCount = 1,
                                            .pQueuePriorities = &priority};
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT
            maintenance{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT, .swapchainMaintenance1 = VK_TRUE};
        constexpr std::array extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
        const VkDeviceCreateInfo info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                      .pNext = &maintenance,
                                      .queueCreateInfoCount = 1,
                                      .pQueueCreateInfos = &queue,
                                      .enabledExtensionCount = extensions.size(),
                                      .ppEnabledExtensionNames = extensions.data()};
        const auto result = create(physical, &info, nullptr, &device_);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateDevice", result));
        physical_ = physical;
        family_ = family;
        vk_.destroyDevice = std::bit_cast<PFN_vkDestroyDevice>(vk_.deviceProc(device_, "vkDestroyDevice"));
        const auto getQueue = std::bit_cast<PFN_vkGetDeviceQueue>(vk_.deviceProc(device_, "vkGetDeviceQueue"));
        if (!vk_.destroyDevice || !getQueue)
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        getQueue(device_, family_, 0, &queue_);
        return LoadDispatch();
    }

    /** @brief Checks exact owner-lane and lifecycle admission before touching native ownership. */
    Result<void> Runtime::Access(bool allowClosed) const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Errors::WrongThread));
        if (!initialized_)
            return Result<void>::Failure(MakeError(Errors::Invalid));
        if (closing_ && !allowClosed)
            return Result<void>::Failure(MakeError(Errors::Closed));
        return Result<void>::Success();
    }

    /** @brief Initializes the admitted device before any window access; failures unwind only acquired native parents. */
    Result<void> Runtime::Initialize(const RenderBackendConfig &config) {
        if (initialized_ || device_)
            return Result<void>::Failure(MakeError(Errors::Invalid));
        if (closing_)
            return Result<void>::Failure(MakeError(Errors::Closed));
        if (owner_ != std::thread::id{} && owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Errors::WrongThread));
        if (!AdmitsConfiguration(config))
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        owner_ = std::this_thread::get_id();
        config_ = config;
        const auto result = PrepareNative();
        if (result.HasError()) {
            Rollback();
            return result;
        }
        const auto incarnation = AcquireRenderDeviceIncarnation();
        if (incarnation.HasError()) {
            Rollback();
            return Result<void>::Failure(incarnation.ErrorValue());
        }
        lifecycle_.emplace(RenderSurfaceLifecycle::Create(incarnation.Value()).Value());
        capabilities.presentsToWindow = true;
        initialized_ = true;
        return Result<void>::Success();
    }

    /** @brief Performs transactional cold native acquisition in parent-before-child order, without window access. */
    Result<void> Runtime::PrepareNative() {
        const auto loader = OpenLoader();
        if (loader.HasError())
            return loader;
        const auto instance = CreateInstance();
        if (instance.HasError())
            return instance;
        const auto device = SelectDevice();
        if (device.HasError())
            return device;
        return CreateSlots();
    }

    /** @brief Releases device-child objects only after successful terminal drain or failed cold initialization. */
    void Runtime::DestroyDevice() noexcept {
        for (auto &slot : slots_) {
            if (slot.acquireFence)
                vk_.destroyFence(device_, slot.acquireFence, nullptr);
            if (slot.commandFence)
                vk_.destroyFence(device_, slot.commandFence, nullptr);
            slot = {};
        }
        if (pool_)
            vk_.destroyPool(device_, pool_, nullptr);
        pool_ = {};
        if (device_ && vk_.destroyDevice)
            vk_.destroyDevice(device_, nullptr);
        device_ = {};
        queue_ = {};
        physical_ = {};
    }

    /** @brief Rolls back cold partial creation without touching unrelated owners. */
    void Runtime::Rollback() noexcept {
        DestroySurface();
        DestroyDevice();
        if (instance_ && vk_.destroyInstance)
            vk_.destroyInstance(instance_, nullptr);
        instance_ = {};
#if defined(__ANDROID__)
        if (library_)
            dlclose(library_);
#endif
        library_ = nullptr;
    }
}  // namespace Horo::Render::AndroidVulkan
