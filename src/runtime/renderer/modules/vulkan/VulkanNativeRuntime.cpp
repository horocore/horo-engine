#include "VulkanNativeRuntime.h"

#include "VulkanRenderBackendErrors.h"
#include "VulkanResourceRuntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::uint32_t MaxEnumeratedNames{4096};
        constexpr std::uint32_t MaxQueueFamilies{256};

        template <typename T, typename Resolver>
        [[nodiscard]] T LoadInstanceFunction(const Resolver resolver, const VkInstance instance, const char *name) {
            return std::bit_cast<T>(resolver(instance, name));
        }

        template <typename T, typename Resolver>
        [[nodiscard]] T LoadDeviceFunction(const Resolver resolver, const VkDevice device, const char *name) {
            return std::bit_cast<T>(resolver(device, name));
        }

        [[nodiscard]] VulkanApiVersion ToApiVersion(const std::uint32_t version) noexcept {
            return VulkanApiVersion{
                .major = VK_API_VERSION_MAJOR(version),
                .minor = VK_API_VERSION_MINOR(version),
                .patch = VK_API_VERSION_PATCH(version),
            };
        }

        [[nodiscard]] RenderAdapterKind ToAdapterKind(const VkPhysicalDeviceType type) noexcept {
            using enum RenderAdapterKind;
            switch (type) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    return Discrete;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    return Integrated;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    return Virtual;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    return Software;
                default:
                    return Unknown;
            }
        }

        [[nodiscard]] Error NativeFailure(const char *operation, const VkResult result) {
            return MakeError(VulkanBackendErrors::NativeCallFailed,
                             std::format("{} failed with VkResult {}.", operation, static_cast<int>(result)));
        }

        [[nodiscard]] Error MissingEntryPoint(const char *name) {
            return MakeError(VulkanBackendErrors::EntryPointMissing, std::format("Required Vulkan entry point is missing: {}", name));
        }

        [[nodiscard]] std::vector<const char *> NativeNames(const std::vector<std::string> &names) {
            std::vector<const char *> native;
            native.reserve(names.size());
            std::ranges::transform(names, std::back_inserter(native), [](const std::string &name) {
                return name.c_str();
            });
            return native;
        }

        class VulkanNativeRuntime final : public IVulkanRuntimePort, public IVulkanResourcePort {  // NOSONAR(cpp:S1448)
        public:
            explicit VulkanNativeRuntime(IVulkanLoaderPort &loaderPort) noexcept : loaderPort_(&loaderPort) {}

            ~VulkanNativeRuntime() override {
                DestroyDevice();
                DestroyInstance();
                ReleaseLoader();
            }

            Result<void> AcquireLoader() override {
                if (loaderAcquired_) {
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::AlreadyInitialized, "Vulkan loader is already acquired."));
                }
                if (Result<void> acquired = loaderPort_->AcquireLoader(); acquired.HasError()) {
                    return acquired;
                }
                getInstanceProcAddr_ = loaderPort_->GetInstanceProcAddr();
                if (getInstanceProcAddr_ == nullptr) {
                    loaderPort_->ReleaseLoader();
                    return Result<void>::Failure(MissingEntryPoint("vkGetInstanceProcAddr"));
                }
                loaderAcquired_ = true;
                return Result<void>::Success();
            }

            Result<VulkanLoaderCapabilities> QueryLoaderCapabilities() override {
                const auto enumerateVersion = LoadInstanceFunction<PFN_vkEnumerateInstanceVersion>(getInstanceProcAddr_, VK_NULL_HANDLE,
                                                                                                   "vkEnumerateInstanceVersion");
                const auto enumerateExtensions =
                    LoadInstanceFunction<PFN_vkEnumerateInstanceExtensionProperties>(getInstanceProcAddr_, VK_NULL_HANDLE,
                                                                                     "vkEnumerateInstanceExtensionProperties");
                const auto enumerateLayers =
                    LoadInstanceFunction<PFN_vkEnumerateInstanceLayerProperties>(getInstanceProcAddr_, VK_NULL_HANDLE,
                                                                                 "vkEnumerateInstanceLayerProperties");
                createInstance_ = LoadInstanceFunction<PFN_vkCreateInstance>(getInstanceProcAddr_, VK_NULL_HANDLE, "vkCreateInstance");
                if (enumerateExtensions == nullptr || enumerateLayers == nullptr || createInstance_ == nullptr) {
                    return Result<VulkanLoaderCapabilities>::Failure(MissingEntryPoint("global instance bootstrap"));
                }

                VulkanLoaderCapabilities capabilities;
                capabilities.hasInstanceVersionQuery = enumerateVersion != nullptr;
                std::uint32_t version{VK_API_VERSION_1_0};
                if (enumerateVersion != nullptr) {
                    const VkResult result = enumerateVersion(&version);
                    if (result != VK_SUCCESS) {
                        return Result<VulkanLoaderCapabilities>::Failure(NativeFailure("vkEnumerateInstanceVersion", result));
                    }
                }
                capabilities.apiVersion = ToApiVersion(version);
                capabilities.standardApiVariant = VK_API_VERSION_VARIANT(version) == 0;
                auto extensions = EnumerateInstanceExtensions(enumerateExtensions);
                if (extensions.HasError()) {
                    return Result<VulkanLoaderCapabilities>::Failure(std::move(extensions).ErrorValue());
                }
                capabilities.instanceExtensions = std::move(extensions).Value();
                auto layers = EnumerateInstanceLayers(enumerateLayers);
                if (layers.HasError()) {
                    return Result<VulkanLoaderCapabilities>::Failure(std::move(layers).ErrorValue());
                }
                capabilities.layers = std::move(layers).Value();
                return Result<VulkanLoaderCapabilities>::Success(std::move(capabilities));
            }

            Result<std::vector<std::string>> RequiredPresentationExtensions() override {
                return loaderPort_->RequiredPresentationExtensions();
            }

            Result<void> CreateInstance(const VulkanInstanceRequest &request) override {
                const std::vector<const char *> extensionNames = NativeNames(request.enabledExtensions);
                const auto extensionCount = static_cast<std::uint32_t>(extensionNames.size());
                const char *validationLayer = "VK_LAYER_KHRONOS_validation";
                const VkApplicationInfo applicationInfo{
                    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                    .pApplicationName = "Horo Engine",
                    .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
                    .pEngineName = "Horo Engine",
                    .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
                    .apiVersion = VK_MAKE_API_VERSION(0, request.apiVersion.major, request.apiVersion.minor, request.apiVersion.patch),
                };
                const VkInstanceCreateInfo createInfo{
                    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                    .pApplicationInfo = &applicationInfo,
                    .enabledLayerCount = request.enableValidation ? 1U : 0U,
                    .ppEnabledLayerNames = request.enableValidation ? &validationLayer : nullptr,
                    .enabledExtensionCount = extensionCount,
                    .ppEnabledExtensionNames = extensionNames.data(),
                };
                if (const VkResult result = createInstance_(&createInfo, nullptr, &instance_); result != VK_SUCCESS) {
                    return Result<void>::Failure(NativeFailure("vkCreateInstance", result));
                }
                return LoadInstanceDispatch();
            }

            Result<VulkanAdapterEnumeration> EnumeratePhysicalDevices(const std::uint32_t maxAdapters) override {
                std::uint32_t deviceCount{0};
                VkResult result = enumeratePhysicalDevices_(instance_, &deviceCount, nullptr);
                if (result != VK_SUCCESS) {
                    return Result<VulkanAdapterEnumeration>::Failure(NativeFailure("vkEnumeratePhysicalDevices(count)", result));
                }
                if (deviceCount > maxAdapters) {
                    return Result<VulkanAdapterEnumeration>::Failure(
                        MakeError(VulkanBackendErrors::EnumerationLimitExceeded, "Physical-device count exceeds the requested bound."));
                }
                std::vector<VkPhysicalDevice> devices(deviceCount);
                result = enumeratePhysicalDevices_(instance_, &deviceCount, devices.data());
                if (result != VK_SUCCESS) {
                    return Result<VulkanAdapterEnumeration>::Failure(NativeFailure("vkEnumeratePhysicalDevices", result));
                }

                VulkanAdapterEnumeration enumeration;
                enumeration.adapters.reserve(devices.size());
                nativeAdapters_.clear();
                for (const VkPhysicalDevice device : devices) {
                    auto candidate = InspectAdapter(device);
                    if (candidate.HasError()) {
                        return Result<VulkanAdapterEnumeration>::Failure(std::move(candidate).ErrorValue());
                    }
                    nativeAdapters_.push_back(NativeAdapter{.id = candidate.Value().id, .device = device});
                    enumeration.adapters.push_back(std::move(candidate).Value());
                }
                enumeration.revision = Detail::VulkanAdapterRevision(enumeration.adapters);
                return Result<VulkanAdapterEnumeration>::Success(std::move(enumeration));
            }

            Result<void> CreateDevice(const VulkanDeviceRequest &request) override {
                const auto selected = std::ranges::find(nativeAdapters_, request.adapter, &NativeAdapter::id);
                if (selected == nativeAdapters_.end()) {
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::AdapterNotFound));
                }
                constexpr float priority{1.0F};
                std::array<VkDeviceQueueCreateInfo, 2> queueInfos{};
                std::uint32_t queueInfoCount{1};
                queueInfos[0] = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                        .queueFamilyIndex = request.graphicsQueueFamily,
                                                        .queueCount = 1,
                                                        .pQueuePriorities = &priority};
                if (request.presentationQueueFamily != request.graphicsQueueFamily) {
                    queueInfos[1] = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                            .queueFamilyIndex = request.presentationQueueFamily,
                                                            .queueCount = 1,
                                                            .pQueuePriorities = &priority};
                    queueInfoCount = 2;
                }
                const std::vector<const char *> extensionNames = NativeNames(request.enabledExtensions);
                VkPhysicalDeviceVulkan13Features features13{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                    .synchronization2 = request.enabledFeatures.synchronization2,
                    .dynamicRendering = request.enabledFeatures.dynamicRendering,
                };
                const VkPhysicalDeviceVulkan12Features features12{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                    .pNext = &features13,
                    .timelineSemaphore = request.enabledFeatures.timelineSemaphore,
                };
                const VkDeviceCreateInfo createInfo{
                    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                    .pNext = &features12,
                    .queueCreateInfoCount = queueInfoCount,
                    .pQueueCreateInfos = queueInfos.data(),
                    .enabledExtensionCount = static_cast<std::uint32_t>(extensionNames.size()),
                    .ppEnabledExtensionNames = extensionNames.data(),
                };
                if (const VkResult result = createDevice_(selected->device, &createInfo, nullptr, &device_); result != VK_SUCCESS) {
                    return Result<void>::Failure(NativeFailure("vkCreateDevice", result));
                }
                if (auto dispatch = LoadDeviceDispatch(request); dispatch.HasError()) {
                    return dispatch;
                }
                VkPhysicalDeviceMemoryProperties memoryProperties{};
                getPhysicalDeviceMemoryProperties_(selected->device, &memoryProperties);
                return resources_.Initialize(memoryProperties, device_, getDeviceProcAddr_);
            }

            IRenderResourceBackend *ResourceBackend() noexcept override {
                return &resources_;
            }

            void DestroyDevice() noexcept override {
                resources_.ShutdownResources();
                if (device_ != VK_NULL_HANDLE && destroyDevice_ != nullptr) {
                    destroyDevice_(device_, nullptr);
                }
                device_ = VK_NULL_HANDLE;
                graphicsQueue_ = VK_NULL_HANDLE;
                presentationQueue_ = VK_NULL_HANDLE;
                destroyDevice_ = nullptr;
            }

            void DestroyInstance() noexcept override {
                DestroyDevice();
                nativeAdapters_.clear();
                if (instance_ != VK_NULL_HANDLE && destroyInstance_ != nullptr) {
                    destroyInstance_(instance_, nullptr);
                }
                instance_ = VK_NULL_HANDLE;
                ClearInstanceDispatch();
            }

            void ReleaseLoader() noexcept override {
                DestroyInstance();
                if (loaderAcquired_) {
                    loaderPort_->ReleaseLoader();
                }
                loaderAcquired_ = false;
                getInstanceProcAddr_ = nullptr;
                createInstance_ = nullptr;
            }

        private:
            struct NativeAdapter {
                RenderAdapterId id;
                VkPhysicalDevice device{VK_NULL_HANDLE};
            };

            template <typename Property, typename Enumerator, typename Name>
            [[nodiscard]] Result<std::vector<std::string>> EnumerateNames(const Enumerator &enumerate, const Name &name,
                                                                          const char *operation) const {
                std::uint32_t count{0};
                VkResult result = enumerate(&count, nullptr);
                if (result != VK_SUCCESS) {
                    return Result<std::vector<std::string>>::Failure(NativeFailure(operation, result));
                }
                if (count > MaxEnumeratedNames) {
                    return Result<std::vector<std::string>>::Failure(MakeError(VulkanBackendErrors::EnumerationLimitExceeded));
                }
                std::vector<Property> properties(count);
                result = enumerate(&count, properties.data());
                if (result != VK_SUCCESS) {
                    return Result<std::vector<std::string>>::Failure(NativeFailure(operation, result));
                }
                std::vector<std::string> names;
                names.reserve(properties.size());
                std::ranges::transform(properties, std::back_inserter(names), name);
                return Result<std::vector<std::string>>::Success(std::move(names));
            }

            template <typename Enumerator>
            [[nodiscard]] Result<std::vector<std::string>> EnumerateInstanceExtensions(const Enumerator enumerate) const {
                return EnumerateNames<VkExtensionProperties>([enumerate](std::uint32_t *count, VkExtensionProperties *properties) {
                    return enumerate(nullptr, count, properties);
                }, [](const VkExtensionProperties &property) {
                    return std::string{property.extensionName};
                }, "vkEnumerateInstanceExtensionProperties");
            }

            template <typename Enumerator>
            [[nodiscard]] Result<std::vector<std::string>> EnumerateInstanceLayers(const Enumerator enumerate) const {
                return EnumerateNames<VkLayerProperties>(enumerate, [](const VkLayerProperties &property) {
                    return std::string{property.layerName};
                }, "vkEnumerateInstanceLayerProperties");
            }

            [[nodiscard]] Result<void> LoadInstanceDispatch() {
                destroyInstance_ = LoadInstanceFunction<PFN_vkDestroyInstance>(getInstanceProcAddr_, instance_, "vkDestroyInstance");
                enumeratePhysicalDevices_ =
                    LoadInstanceFunction<PFN_vkEnumeratePhysicalDevices>(getInstanceProcAddr_, instance_, "vkEnumeratePhysicalDevices");
                getPhysicalDeviceProperties2_ = LoadInstanceFunction<PFN_vkGetPhysicalDeviceProperties2>(getInstanceProcAddr_, instance_,
                                                                                                         "vkGetPhysicalDeviceProperties2");
                getPhysicalDeviceFeatures2_ =
                    LoadInstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>(getInstanceProcAddr_, instance_, "vkGetPhysicalDeviceFeatures2");
                getPhysicalDeviceMemoryProperties_ =
                    LoadInstanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>(getInstanceProcAddr_, instance_,
                                                                                  "vkGetPhysicalDeviceMemoryProperties");
                enumerateDeviceExtensions_ =
                    LoadInstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>(getInstanceProcAddr_, instance_,
                                                                                   "vkEnumerateDeviceExtensionProperties");
                getQueueFamilyProperties_ =
                    LoadInstanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(getInstanceProcAddr_, instance_,
                                                                                       "vkGetPhysicalDeviceQueueFamilyProperties");
                createDevice_ = LoadInstanceFunction<PFN_vkCreateDevice>(getInstanceProcAddr_, instance_, "vkCreateDevice");
                getDeviceProcAddr_ = LoadInstanceFunction<PFN_vkGetDeviceProcAddr>(getInstanceProcAddr_, instance_, "vkGetDeviceProcAddr");
                if (destroyInstance_ == nullptr || enumeratePhysicalDevices_ == nullptr || getPhysicalDeviceProperties2_ == nullptr ||
                    getPhysicalDeviceFeatures2_ == nullptr || getPhysicalDeviceMemoryProperties_ == nullptr ||
                    enumerateDeviceExtensions_ == nullptr || getQueueFamilyProperties_ == nullptr || createDevice_ == nullptr ||
                    getDeviceProcAddr_ == nullptr) {
                    return Result<void>::Failure(MissingEntryPoint("required Vulkan 1.3 instance dispatch"));
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<VulkanAdapterCandidate> InspectAdapter(const VkPhysicalDevice device) {
                VkPhysicalDeviceIDProperties identity{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &identity};
                getPhysicalDeviceProperties2_(device, &properties);

                VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
                VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                            .pNext = &features13};
                VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
                getPhysicalDeviceFeatures2_(device, &features);

                auto extensions = EnumerateDeviceExtensions(device);
                if (extensions.HasError()) {
                    return Result<VulkanAdapterCandidate>::Failure(std::move(extensions).ErrorValue());
                }
                auto queues = EnumerateQueues(device);
                if (queues.HasError()) {
                    return Result<VulkanAdapterCandidate>::Failure(std::move(queues).ErrorValue());
                }
                auto driverAllowed = loaderPort_->IsDriverAllowed(properties.properties);
                if (driverAllowed.HasError()) {
                    return Result<VulkanAdapterCandidate>::Failure(std::move(driverAllowed).ErrorValue());
                }
                const bool portabilitySubset =
                    std::ranges::find(extensions.Value(), "VK_KHR_portability_subset") != extensions.Value().end();
                return Result<VulkanAdapterCandidate>::Success(VulkanAdapterCandidate{
                    .id = RenderAdapterId{Detail::VulkanAdapterIdentity(identity)},
                    .displayName = properties.properties.deviceName,
                    .kind = ToAdapterKind(properties.properties.deviceType),
                    .dedicatedVideoMemoryBytes = DedicatedVideoMemory(device, properties.properties.deviceType),
                    .apiVersion = ToApiVersion(properties.properties.apiVersion),
                    .driverVersion = properties.properties.driverVersion,
                    .requiredFeatures = {.dynamicRendering = features13.dynamicRendering == VK_TRUE,
                                         .synchronization2 = features13.synchronization2 == VK_TRUE,
                                         .timelineSemaphore = features12.timelineSemaphore == VK_TRUE},
                    .deviceExtensions = std::move(extensions).Value(),
                    .queueFamilies = std::move(queues).Value(),
                    .standardApiVariant = VK_API_VERSION_VARIANT(properties.properties.apiVersion) == 0,
                    .portabilitySubset = portabilitySubset,
                    .driverAllowed = driverAllowed.Value(),
                });
            }

            [[nodiscard]] std::uint64_t DedicatedVideoMemory(const VkPhysicalDevice device, const VkPhysicalDeviceType type) const {
                if (type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    return 0;
                }
                VkPhysicalDeviceMemoryProperties properties{};
                getPhysicalDeviceMemoryProperties_(device, &properties);
                std::uint64_t total{0};
                for (std::uint32_t index = 0; index < properties.memoryHeapCount; ++index) {
                    if ((properties.memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                        const std::uint64_t heapSize = properties.memoryHeaps[index].size;
                        total = heapSize > std::numeric_limits<std::uint64_t>::max() - total ? std::numeric_limits<std::uint64_t>::max()
                                                                                             : total + heapSize;
                    }
                }
                return total;
            }

            [[nodiscard]] Result<std::vector<std::string>> EnumerateDeviceExtensions(const VkPhysicalDevice device) const {
                return EnumerateNames<VkExtensionProperties>([this, device](std::uint32_t *count, VkExtensionProperties *properties) {
                    return enumerateDeviceExtensions_(device, nullptr, count, properties);
                }, [](const VkExtensionProperties &property) {
                    return std::string{property.extensionName};
                }, "vkEnumerateDeviceExtensionProperties");
            }

            [[nodiscard]] Result<std::vector<VulkanQueueFamily>> EnumerateQueues(const VkPhysicalDevice device) const {
                std::uint32_t count{0};
                getQueueFamilyProperties_(device, &count, nullptr);
                if (count > MaxQueueFamilies) {
                    return Result<std::vector<VulkanQueueFamily>>::Failure(MakeError(VulkanBackendErrors::EnumerationLimitExceeded));
                }
                std::vector<VkQueueFamilyProperties> properties(count);
                getQueueFamilyProperties_(device, &count, properties.data());
                std::vector<VulkanQueueFamily> queues;
                queues.reserve(properties.size());
                for (std::uint32_t index = 0; index < properties.size(); ++index) {
                    auto presentation = loaderPort_->SupportsPresentation(device, index);
                    if (presentation.HasError()) {
                        return Result<std::vector<VulkanQueueFamily>>::Failure(std::move(presentation).ErrorValue());
                    }
                    queues.push_back(VulkanQueueFamily{
                        .index = index,
                        .supportsGraphics = properties[index].queueCount > 0 && (properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0,
                        .supportsPresentation = properties[index].queueCount > 0 && presentation.Value(),
                    });
                }
                return Result<std::vector<VulkanQueueFamily>>::Success(std::move(queues));
            }

            [[nodiscard]] Result<void> LoadDeviceDispatch(const VulkanDeviceRequest &request) {
                destroyDevice_ = LoadDeviceFunction<PFN_vkDestroyDevice>(getDeviceProcAddr_, device_, "vkDestroyDevice");
                const auto getDeviceQueue = LoadDeviceFunction<PFN_vkGetDeviceQueue>(getDeviceProcAddr_, device_, "vkGetDeviceQueue");
                if (destroyDevice_ == nullptr || getDeviceQueue == nullptr) {
                    return Result<void>::Failure(MissingEntryPoint("required Vulkan device dispatch"));
                }
                getDeviceQueue(device_, request.graphicsQueueFamily, 0, &graphicsQueue_);
                getDeviceQueue(device_, request.presentationQueueFamily, 0, &presentationQueue_);
                if (graphicsQueue_ == VK_NULL_HANDLE || presentationQueue_ == VK_NULL_HANDLE) {
                    return Result<void>::Failure(
                        MakeError(VulkanBackendErrors::NativeCallFailed, "Vulkan returned a null required queue."));
                }
                return Result<void>::Success();
            }

            void ClearInstanceDispatch() noexcept {
                destroyInstance_ = nullptr;
                enumeratePhysicalDevices_ = nullptr;
                getPhysicalDeviceProperties2_ = nullptr;
                getPhysicalDeviceFeatures2_ = nullptr;
                getPhysicalDeviceMemoryProperties_ = nullptr;
                enumerateDeviceExtensions_ = nullptr;
                getQueueFamilyProperties_ = nullptr;
                createDevice_ = nullptr;
                getDeviceProcAddr_ = nullptr;
            }

            IVulkanLoaderPort *loaderPort_{nullptr};
            bool loaderAcquired_{false};
            PFN_vkGetInstanceProcAddr getInstanceProcAddr_{nullptr};
            PFN_vkCreateInstance createInstance_{nullptr};
            VkInstance instance_{VK_NULL_HANDLE};
            PFN_vkDestroyInstance destroyInstance_{nullptr};
            PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices_{nullptr};
            PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2_{nullptr};
            PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2_{nullptr};
            PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties_{nullptr};
            PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensions_{nullptr};
            PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilyProperties_{nullptr};
            PFN_vkCreateDevice createDevice_{nullptr};
            PFN_vkGetDeviceProcAddr getDeviceProcAddr_{nullptr};
            std::vector<NativeAdapter> nativeAdapters_;
            VkDevice device_{VK_NULL_HANDLE};
            VkQueue graphicsQueue_{VK_NULL_HANDLE};
            VkQueue presentationQueue_{VK_NULL_HANDLE};
            PFN_vkDestroyDevice destroyDevice_{nullptr};
            Detail::VulkanResourceRuntime resources_;
        };
    }  // namespace

    namespace Detail {
        std::shared_ptr<IVulkanRuntimePort> CreateVulkanNativeRuntime(IVulkanLoaderPort &loaderPort) {
            return std::make_shared<VulkanNativeRuntime>(loaderPort);
        }
    }  // namespace Detail
}  // namespace Horo::Render
