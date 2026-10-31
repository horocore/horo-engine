#include "VulkanNativeRuntime.h"

#include "VulkanGraphRuntime.h"
#include "VulkanMultiviewDepthSupport.h"
#include "VulkanPresentationRuntime.h"
#include "VulkanRenderBackendErrors.h"
#include "VulkanResourceConversions.h"
#include "VulkanResourceRuntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
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

        class VulkanNativeRuntime final : public IVulkanRuntimePort,
                                          public IVulkanResourcePort,
                                          public IVulkanCommandPort,
                                          public Detail::IVulkanEditorGraphicsPort {  // NOSONAR(cpp:S1448)
        public:
            explicit VulkanNativeRuntime(IVulkanLoaderPort &loaderPort, Detail::VulkanEditorGraphicsBridge *bridge) noexcept
                : loaderPort_(&loaderPort), editorBridge_(bridge) {}

            ~VulkanNativeRuntime() override {
                DestroyDevice();
                DestroyInstance();
                ReleaseLoader();
                if (editorBridge_)
                    editorBridge_->Detach(*this);
            }

            Result<void> AcquireLoader() override {
                if (loaderAcquired_) {
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::AlreadyInitialized, "Vulkan loader is already acquired."));
                }
                if (Result<void> acquired = loaderPort_->AcquireLoader(); acquired.HasError()) {
                    return acquired;
                }
                dispatch_.getInstanceProcAddr = loaderPort_->GetInstanceProcAddr();
                if (dispatch_.getInstanceProcAddr == nullptr) {
                    loaderPort_->ReleaseLoader();
                    return Result<void>::Failure(MissingEntryPoint("vkGetInstanceProcAddr"));
                }
                loaderAcquired_ = true;
                auto requirements = loaderPort_->RequiredGraphicsRequirements();
                if (requirements.HasError())
                    return Result<void>::Failure(requirements.ErrorValue());
                enabledGraphics_.host.emplace(std::move(requirements).Value());
                return Result<void>::Success();
            }

            Result<VulkanHostGraphicsRequirements> RequiredGraphicsRequirements() override {
                if (!loaderAcquired_ || !enabledGraphics_.host)
                    return Result<VulkanHostGraphicsRequirements>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
                return Result<VulkanHostGraphicsRequirements>::Success(*enabledGraphics_.host);
            }

            Result<VulkanLoaderCapabilities> QueryLoaderCapabilities() override {
                const auto enumerateVersion =
                    LoadInstanceFunction<PFN_vkEnumerateInstanceVersion>(dispatch_.getInstanceProcAddr, VK_NULL_HANDLE,
                                                                         "vkEnumerateInstanceVersion");
                const auto enumerateExtensions =
                    LoadInstanceFunction<PFN_vkEnumerateInstanceExtensionProperties>(dispatch_.getInstanceProcAddr, VK_NULL_HANDLE,
                                                                                     "vkEnumerateInstanceExtensionProperties");
                const auto enumerateLayers =
                    LoadInstanceFunction<PFN_vkEnumerateInstanceLayerProperties>(dispatch_.getInstanceProcAddr, VK_NULL_HANDLE,
                                                                                 "vkEnumerateInstanceLayerProperties");
                dispatch_.createInstance =
                    LoadInstanceFunction<PFN_vkCreateInstance>(dispatch_.getInstanceProcAddr, VK_NULL_HANDLE, "vkCreateInstance");
                if (enumerateExtensions == nullptr || enumerateLayers == nullptr || dispatch_.createInstance == nullptr) {
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
                if (instance_ != VK_NULL_HANDLE)
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::AlreadyInitialized));
                // Native/host callbacks can mutate the caller's request; publication retains this admitted value.
                const auto admitted = request;
                if (!enabledGraphics_.host)
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
                if (!enabledGraphics_.host->Allows(admitted.apiVersion))
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::LoaderVersionUnsupported));
                if (!std::ranges::all_of(enabledGraphics_.host->Instance(), [&admitted](const auto &name) {
                    return std::ranges::find(admitted.enabledExtensions, name) != admitted.enabledExtensions.end();
                }))
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::InstanceExtensionMissing));
                const std::vector<const char *> extensionNames = NativeNames(admitted.enabledExtensions);
                const auto extensionCount = static_cast<std::uint32_t>(extensionNames.size());
                const char *validationLayer = "VK_LAYER_KHRONOS_validation";
                const VkApplicationInfo applicationInfo{
                    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                    .pApplicationName = "Horo Engine",
                    .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
                    .pEngineName = "Horo Engine",
                    .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
                    .apiVersion = VK_MAKE_API_VERSION(0, admitted.apiVersion.major, admitted.apiVersion.minor, admitted.apiVersion.patch),
                };
                const VkInstanceCreateInfo createInfo{
                    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                    .pApplicationInfo = &applicationInfo,
                    .enabledLayerCount = admitted.enableValidation ? 1U : 0U,
                    .ppEnabledLayerNames = admitted.enableValidation ? &validationLayer : nullptr,
                    .enabledExtensionCount = extensionCount,
                    .ppEnabledExtensionNames = extensionNames.data(),
                };
                if (const VkResult result = dispatch_.createInstance(&createInfo, nullptr, &instance_); result != VK_SUCCESS) {
                    return Result<void>::Failure(NativeFailure("vkCreateInstance", result));
                }
                if (auto loaded = LoadInstanceDispatch(); loaded.HasError())
                    return loaded;
                if (std::ranges::find(admitted.enabledExtensions, VK_KHR_SURFACE_EXTENSION_NAME) != admitted.enabledExtensions.end()) {
                    auto created = loaderPort_->CreatePresentationSurface(instance_);
                    if (created.HasError())
                        return Result<void>::Failure(created.ErrorValue());
                    if (created.Value() == VK_NULL_HANDLE)
                        return Result<void>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
                    surface_ = created.Value();
                }
                enabledGraphics_.api = admitted.apiVersion;
                enabledGraphics_.instanceExtensions = admitted.enabledExtensions;
                return Result<void>::Success();
            }

            Result<VulkanAdapterEnumeration> EnumeratePhysicalDevices(const std::uint32_t maxAdapters) override {
                std::uint32_t deviceCount{0};
                VkResult result = dispatch_.enumeratePhysicalDevices(instance_, &deviceCount, nullptr);
                if (result != VK_SUCCESS) {
                    return Result<VulkanAdapterEnumeration>::Failure(NativeFailure("vkEnumeratePhysicalDevices(count)", result));
                }
                if (deviceCount > maxAdapters) {
                    return Result<VulkanAdapterEnumeration>::Failure(
                        MakeError(VulkanBackendErrors::EnumerationLimitExceeded, "Physical-device count exceeds the requested bound."));
                }
                std::vector<VkPhysicalDevice> devices(deviceCount);
                result = dispatch_.enumeratePhysicalDevices(instance_, &deviceCount, devices.data());
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

            /** @brief Deduplicates native queue families without changing requested role assignments. */
            static std::uint32_t PrepareDeviceQueues(const VulkanDeviceRequest &request, const float &priority,
                                                     std::array<VkDeviceQueueCreateInfo, 3> &queueInfos) {
                std::uint32_t queueInfoCount{0};
                const std::array families{request.graphicsQueueFamily, request.presentationQueueFamily, request.transferQueueFamily};
                for (const auto family : families) {
                    if (std::ranges::any_of(std::span{queueInfos}.first(queueInfoCount), [family](const VkDeviceQueueCreateInfo &info) {
                        return info.queueFamilyIndex == family;
                    }))
                        continue;
                    queueInfos[queueInfoCount++] = VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                                           .queueFamilyIndex = family,
                                                                           .queueCount = 1,
                                                                           .pQueuePriorities = &priority};
                }
                return queueInfoCount;
            }

            /** @brief Revalidates the same-instance runtime device and exact required extension request before native creation. */
            Result<void> ValidateHostDevice(const VkPhysicalDevice device, const VulkanDeviceRequest &request) const {
                const auto allowed = loaderPort_->AcceptPhysicalDevice(instance_, device);
                if (allowed.HasError())
                    return Result<void>::Failure(allowed.ErrorValue());
                if (!allowed.Value() || !enabledGraphics_.host)
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::AdapterRequirementsUnsupported));
                if (!std::ranges::all_of(enabledGraphics_.host->Device(), [&request](const auto &name) {
                    return std::ranges::find(request.enabledExtensions, name) != request.enabledExtensions.end();
                }))
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::DeviceExtensionMissing));
                return Result<void>::Success();
            }

            Result<void> CreateDevice(const VulkanDeviceRequest &request) override {
                if (device_ != VK_NULL_HANDLE)
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::AlreadyInitialized));
                // Keep owned extension/queue storage stable across host validation and native creation callbacks.
                const auto admitted = request;
                const auto selected = std::ranges::find(nativeAdapters_, admitted.adapter, &NativeAdapter::id);
                if (selected == nativeAdapters_.end()) {
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::AdapterNotFound));
                }
                if (const auto host = ValidateHostDevice(selected->device, admitted); host.HasError())
                    return host;
                constexpr float priority{1.0F};
                std::array<VkDeviceQueueCreateInfo, 3> queueInfos{};
                const auto queueInfoCount = PrepareDeviceQueues(admitted, priority, queueInfos);
                const std::vector<const char *> extensionNames = NativeNames(admitted.enabledExtensions);
                VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT
                    maintenance{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
                                .swapchainMaintenance1 = admitted.enableSwapchainMaintenance};
                VkPhysicalDeviceVulkan13Features features13{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                    .pNext = admitted.enableSwapchainMaintenance ? &maintenance : nullptr,
                    .synchronization2 = admitted.enabledFeatures.synchronization2,
                    .dynamicRendering = admitted.enabledFeatures.dynamicRendering,
                };
                const VkPhysicalDeviceVulkan12Features features12{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                    .pNext = &features13,
                    .timelineSemaphore = admitted.enabledFeatures.timelineSemaphore,
                    .shaderOutputLayer = admitted.enableVertexLayerRouting,
                };
                const VkDeviceCreateInfo createInfo{
                    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                    .pNext = &features12,
                    .queueCreateInfoCount = queueInfoCount,
                    .pQueueCreateInfos = queueInfos.data(),
                    .enabledExtensionCount = static_cast<std::uint32_t>(extensionNames.size()),
                    .ppEnabledExtensionNames = extensionNames.data(),
                };
                if (const VkResult result = dispatch_.createDevice(selected->device, &createInfo, nullptr, &device_);
                    result != VK_SUCCESS) {
                    return Result<void>::Failure(NativeFailure("vkCreateDevice", result));
                }
                if (auto dispatch = LoadDeviceDispatch(admitted); dispatch.HasError()) {
                    return dispatch;
                }
                auto initialized = InitializeResources(selected->device, admitted);
                if (initialized.HasValue())
                    enabledGraphics_.deviceExtensions = admitted.enabledExtensions;
                return initialized;
            }

            /** @brief Binds borrowed editor device facts and validates their canonical graphics queue role. */
            Result<void> BindEditorDevice(const VkPhysicalDevice physical, const VulkanDeviceRequest &request) {
                editorPhysical_ = physical;
                editorGraphicsFamily_ = request.graphicsQueueFamily;
                editorFrames_ = request.maxFramesInFlight;
                const auto graphicsAssignment =
                    std::ranges::find(request.queueAssignments, RenderQueueRole::Graphics, &RenderQueueAssignment::role);
                if (graphicsAssignment == request.queueAssignments.end())
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::GraphicsQueueUnavailable));
                editorGraphicsIdentity_ = graphicsAssignment->queue;
                return Result<void>::Success();
            }

            /** @brief Attaches the sole resource registry and shader owner to the selected native device. */
            Result<void> InitializeResources(VkPhysicalDevice physical, const VulkanDeviceRequest &request) {
                if (auto bound = BindEditorDevice(physical, request); bound.HasError())
                    return bound;
                VkPhysicalDeviceMemoryProperties memoryProperties{};
                dispatch_.getPhysicalDeviceMemoryProperties(physical, &memoryProperties);
                if (auto resources = resources_.Initialize(memoryProperties, device_, dispatch_.getDeviceProcAddr); resources.HasError())
                    return resources;
                VkPhysicalDeviceMaintenance3Properties allocationLimits{.sType =
                                                                            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
                VkPhysicalDeviceTimelineSemaphoreProperties timeline{.sType =
                                                                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES,
                                                                     .pNext = &allocationLimits};
                VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &timeline};
                dispatch_.getPhysicalDeviceProperties2(physical, &properties);
                std::vector<Detail::VulkanCommandQueue> queues;
                for (const auto &assignment : request.queueAssignments) {
                    if (std::ranges::any_of(queues, [&assignment](const Detail::VulkanCommandQueue &queue) {
                        return queue.identity == assignment.queue;
                    }))
                        continue;
                    const bool graphics =
                        assignment.role == RenderQueueRole::Graphics || request.transferQueueFamily == request.graphicsQueueFamily;
                    queues.emplace_back(assignment.queue, graphics ? request.graphicsQueueFamily : request.transferQueueFamily,
                                        graphics ? graphicsQueue_ : transferQueue_, graphics);
                }
                const auto incarnation = AcquireRenderDeviceIncarnation();
                if (incarnation.HasError())
                    return Result<void>::Failure(incarnation.ErrorValue());
                if (auto shaders = resources_.InitializeShaders(properties.properties, request.adapter, dispatch_.getDeviceProcAddr,
                                                                incarnation.Value(), request.enableVertexLayerRouting);
                    shaders.HasError())
                    return shaders;
                if (auto prepared = graph_.Initialize({.device = device_,
                                                       .queues = queues,
                                                       .frames = request.maxFramesInFlight,
                                                       .timelineDifference = timeline.maxTimelineSemaphoreValueDifference,
                                                       .deviceIncarnation = incarnation.Value(),
                                                       .resources = resources_,
                                                       .resolver = dispatch_.getDeviceProcAddr,
                                                       .vertexLayerEnabled = request.enableVertexLayerRouting});
                    prepared.HasError())
                    return prepared;
                if (auto published = PublishResourceCapabilities(physical, properties.properties, allocationLimits.maxMemoryAllocationSize,
                                                                 incarnation.Value());
                    published.HasError())
                    return published;
                return request.enableSwapchainMaintenance ? InitializePresentation(physical, request, incarnation.Value())
                                                          : Result<void>::Success();
            }

            /** @brief Binds WSI retirement to the exact canonical graph completion owner after native resource admission. */
            Result<void> InitializePresentation(VkPhysicalDevice physical, const VulkanDeviceRequest &request,
                                                const std::uint64_t incarnation) {
                if (!surface_)
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
                observePresentation_ = [this, deviceIncarnation = incarnation](const RenderTimelinePoint completion) {
                    return graph_.ObserveSubmission({.completion = completion, .expectedDeviceIncarnation = deviceIncarnation});
                };
                presentation_ = std::make_unique<Detail::VulkanPresentationRuntime>();
                return presentation_->Initialize({.instance = instance_,
                                                  .physical = physical,
                                                  .device = device_,
                                                  .surface = surface_,
                                                  .presentQueue = presentationQueue_,
                                                  .graphicsFamily = request.graphicsQueueFamily,
                                                  .presentFamily = request.presentationQueueFamily,
                                                  .frames = request.maxFramesInFlight,
                                                  .instanceResolver = dispatch_.getInstanceProcAddr,
                                                  .deviceResolver = dispatch_.getDeviceProcAddr,
                                                  .observeCompletion = observePresentation_});
            }

            bool SupportsWindowPresentation() const noexcept override {
                return presentation_ != nullptr;
            }

            Result<bool> PollPresentationAvailability(const FramebufferExtent extent) override {
                return presentation_ ? presentation_->PollAvailability(extent) : IVulkanCommandPort::PollPresentationAvailability(extent);
            }

            Result<void> ResizePresentation(const FramebufferExtent extent, const PresentMode mode) override {
                return presentation_ ? presentation_->Resize(extent, mode) : IVulkanCommandPort::ResizePresentation(extent, mode);
            }

            IRenderResourceBackend *ResourceBackend() noexcept override {
                return &resources_;
            }

            RenderCapabilitySnapshot ResourceCapabilities() const noexcept override {
                return resourceCapabilities_;
            }

            Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
                if (!descriptor.outputExtent.IsValid() || nextFrame_ == std::numeric_limits<std::uint64_t>::max())
                    return Result<FrameToken>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
                const FrameToken frame{nextFrame_};
                if (auto begun = graph_.BeginFrame(frame); begun.HasError())
                    return Result<FrameToken>::Failure(std::move(begun).ErrorValue());
                if (presentation_) {
                    const auto output = presentation_->BeginFrame(frame);
                    if (output.HasError()) {
                        graph_.AbortFrame(frame);
                        return Result<FrameToken>::Failure(output.ErrorValue());
                    }
                    presentationFrame_ = output.Value();
                    presentationFrameToken_ = frame;
                    presentationCompletion_ = {};
                    primaryOutputPrepared_ = false;
                }
                ++nextFrame_;
                return Result<FrameToken>::Success(frame);
            }

            Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request) override {
                return graph_.ExecuteGraph(request);
            }

            bool SupportsMaterialCommands() const noexcept override {
                return true;
            }

            Result<void> ExecuteMaterialPass(const RenderMaterialExecutionRequest &request) override {
                return graph_.ExecuteMaterialPass(request);
            }

            Result<void> UpdateVfxFrame(const NativeVfxFrameUpdate &update) override {
                return graph_.UpdateVfxFrame(update);
            }

#include "VulkanNativeEditorDelegation.inl"

            Result<void> SubmitFrame(const FrameToken frame) override {
                if (presentation_ && (presentationFrameToken_ != frame || !presentationCompletion_.IsValid()))
                    return Result<void>::Failure(
                        MakeError(VulkanBackendErrors::InvalidCommandState,
                                  "Interactive Vulkan requires the matching GUI/viewport tail before presentation."));
                if (const auto submitted = graph_.SubmitFrame(frame); submitted.HasError())
                    return submitted;
                if (!presentation_)
                    return Result<void>::Success();
                // SYNC: Successful tail submission publishes its exact completion before any present or recovery path.
                if (const auto retained = presentation_->Submitted(frame, presentationCompletion_); retained.HasError())
                    std::terminate();
                presentationFrameToken_ = {};
                presentationCompletion_ = {};
                return presentation_->Present(frame);
            }

            void AbortFrame(const FrameToken frame) noexcept override {
                graph_.AbortFrame(frame);
                if (presentation_)
                    presentation_->Abort(frame);
                if (frame == presentationFrameToken_) {
                    presentationFrameToken_ = {};
                    presentationCompletion_ = {};
                }
            }

            Result<bool> ObserveSubmission(const RenderCpuWait &wait) override {
                return graph_.ObserveSubmission(wait);
            }

            Result<void> BeginShutdownCommands() override {
                if (presentation_)
                    if (auto closed = presentation_->BeginShutdown(); closed.HasError())
                        return closed;
                return graph_.BeginShutdown();
            }

            Result<RenderBackendDrainProgress> PollCommandDrain() override {
                if (presentation_)
                    if (auto retired = presentation_->PollDrain();
                        retired.HasError() || retired.Value() != RenderBackendDrainProgress::Drained)
                        return retired;
                return graph_.PollDrain();
            }

            void DestroyDevice() noexcept override {
                if (presentation_) {
                    const auto closed = presentation_->BeginShutdown();
                    const auto drained = presentation_->PollDrain();
                    if (closed.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                        std::terminate();
                    presentation_.reset();
                    observePresentation_ = {};
                }
                // Private rollback is legal only after explicit terminal drain or before any submission.
                const auto closing = graph_.BeginShutdown();
                if (const auto drained = graph_.PollDrain();
                    closing.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                    std::terminate();
                for (auto &child : editorChildren_)
                    if (auto *owned = std::exchange(child, nullptr))
                        owned->ReleaseNative();
                resources_.ShutdownResources();
                resourceCapabilities_ = {};
                if (device_ != VK_NULL_HANDLE && dispatch_.destroyDevice != nullptr) {
                    dispatch_.destroyDevice(device_, nullptr);
                }
                device_ = VK_NULL_HANDLE;
                enabledGraphics_.deviceExtensions.clear();
                graphicsQueue_ = VK_NULL_HANDLE;
                presentationQueue_ = VK_NULL_HANDLE;
                transferQueue_ = VK_NULL_HANDLE;
                dispatch_.destroyDevice = nullptr;
            }

            void DestroyInstance() noexcept override {
                DestroyDevice();
                nativeAdapters_.clear();
                if (surface_ != VK_NULL_HANDLE) {
                    loaderPort_->DestroyPresentationSurface(instance_, surface_);
                    surface_ = VK_NULL_HANDLE;
                }
                if (instance_ != VK_NULL_HANDLE && dispatch_.destroyInstance != nullptr) {
                    dispatch_.destroyInstance(instance_, nullptr);
                }
                instance_ = VK_NULL_HANDLE;
                enabledGraphics_.instanceExtensions.clear();
                enabledGraphics_.api = {};
                ClearInstanceDispatch();
            }

            void ReleaseLoader() noexcept override {
                DestroyInstance();
                if (loaderAcquired_) {
                    loaderPort_->ReleaseLoader();
                }
                loaderAcquired_ = false;
                enabledGraphics_.host.reset();
                dispatch_.getInstanceProcAddr = nullptr;
                dispatch_.createInstance = nullptr;
            }

        private:
            struct NativeAdapter {
                RenderAdapterId id;
                VkPhysicalDevice device{VK_NULL_HANDLE};
            };

            /** @brief Intersects actual enabled draw/layer dispatch with the selected device's framebuffer layer limit. */
            Result<void> PublishMultiviewCapabilities(const VkPhysicalDevice device, const VkPhysicalDeviceProperties &properties) {
                auto &snapshot = resourceCapabilities_;
                const auto multiview = graph_.MultiviewSupport();
                if (multiview.indexedShaders) {
                    snapshot.features.Enable(RenderCapability::ViewIndexedShaders);
                    const RenderTextureDescriptor depth{.extent = {1, 1},
                                                        .format = RenderTextureFormat::Depth32Float,
                                                        .usage = RenderTextureUsage::RenderAttachment};
                    if (snapshot.formats.Supports(depth)) {
                        const auto query =
                            LoadInstanceFunction<PFN_vkGetPhysicalDeviceImageFormatProperties>(dispatch_.getInstanceProcAddr, instance_,
                                                                                               "vkGetPhysicalDeviceImageFormatProperties");
                        const auto admitted = Detail::QueryMultiviewDepthLimits(device, query, multiview.maximumViews, properties.limits);
                        if (admitted.HasError()) {
                            snapshot = {};
                            return Result<void>::Failure(admitted.ErrorValue());
                        }
                        snapshot.limits.maxMultiviewViews = admitted.Value().maximumViews;
                        if (admitted.Value().maximumViews != 0)
                            snapshot.limits.maxTextureDimension2D =
                                std::min(snapshot.limits.maxTextureDimension2D, admitted.Value().maximumExtent2D);
                    }
                }
                if (multiview.instancedTargetRouting)
                    snapshot.features.Enable(RenderCapability::InstancedViewRouting);
                return Result<void>::Success();
            }

            /** @brief Publishes queried device/format facts, intersected with this concrete implementation's resource policy. */
            Result<void> PublishResourceCapabilities(const VkPhysicalDevice device, const VkPhysicalDeviceProperties &properties,
                                                     const VkDeviceSize allocationLimit, const std::uint64_t incarnation) {
                using enum RenderTextureFormat;
                auto &snapshot = resourceCapabilities_;
                snapshot = {.deviceIncarnation = incarnation,
                            .capabilityRevision = 1,
                            .synthetic = false,
                            .features = {},
                            .queues = {.graphics = true, .compute = false, .copy = true, .present = false},
                            .limits = {.maxBufferBytes = allocationLimit,
                                       .maxTextureDimension2D = properties.limits.maxImageDimension2D,
                                       .maxColorAttachments = std::min(1U, properties.limits.maxColorAttachments),
                                       .maxVertexAttributes = properties.limits.maxVertexInputAttributes,
                                       .maxFramesInFlight = 8},
                            .formats = {}};
                for (const auto capability :
                     {RenderCapability::BufferResources, RenderCapability::MeshResources, RenderCapability::TextureResources,
                      RenderCapability::RenderTargetResources, RenderCapability::OffscreenTargets})
                    snapshot.features.Enable(capability);
                const auto query = LoadInstanceFunction<PFN_vkGetPhysicalDeviceFormatProperties>(dispatch_.getInstanceProcAddr, instance_,
                                                                                                 "vkGetPhysicalDeviceFormatProperties");
                if (query == nullptr) {
                    snapshot = {};
                    return Result<void>::Failure(
                        MakeError(VulkanBackendErrors::EntryPointMissing,
                                  "vkGetPhysicalDeviceFormatProperties is required to publish native resource capabilities."));
                }
                snapshot.formats.sampleCountMask = std::uint64_t{1} << 1U;
                for (const auto format : {Rgba8Unorm, Depth24Stencil8, Depth32Float}) {
                    using enum RenderTextureUsage;
                    VkFormatProperties native{};
                    query(device, Detail::VulkanResourceConversion::TextureFormat(format), &native);
                    auto usage = None;
                    if ((native.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0)
                        usage = usage | Sampled;
                    if ((native.optimalTilingFeatures &
                         (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0)
                        usage = usage | RenderAttachment;
                    if ((native.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0)
                        usage = usage | CopySource;
                    if ((native.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0)
                        usage = usage | CopyDestination;
                    snapshot.formats.usages[static_cast<std::size_t>(format)] = usage;
                }
                return PublishMultiviewCapabilities(device, properties);
            }

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
                dispatch_.destroyInstance =
                    LoadInstanceFunction<PFN_vkDestroyInstance>(dispatch_.getInstanceProcAddr, instance_, "vkDestroyInstance");
                dispatch_.enumeratePhysicalDevices =
                    LoadInstanceFunction<PFN_vkEnumeratePhysicalDevices>(dispatch_.getInstanceProcAddr, instance_,
                                                                         "vkEnumeratePhysicalDevices");
                dispatch_.getPhysicalDeviceProperties2 =
                    LoadInstanceFunction<PFN_vkGetPhysicalDeviceProperties2>(dispatch_.getInstanceProcAddr, instance_,
                                                                             "vkGetPhysicalDeviceProperties2");
                dispatch_.getPhysicalDeviceFeatures2 =
                    LoadInstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>(dispatch_.getInstanceProcAddr, instance_,
                                                                           "vkGetPhysicalDeviceFeatures2");
                dispatch_.getPhysicalDeviceMemoryProperties =
                    LoadInstanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>(dispatch_.getInstanceProcAddr, instance_,
                                                                                  "vkGetPhysicalDeviceMemoryProperties");
                dispatch_.enumerateDeviceExtensions =
                    LoadInstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>(dispatch_.getInstanceProcAddr, instance_,
                                                                                   "vkEnumerateDeviceExtensionProperties");
                dispatch_.getQueueFamilyProperties =
                    LoadInstanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(dispatch_.getInstanceProcAddr, instance_,
                                                                                       "vkGetPhysicalDeviceQueueFamilyProperties");
                dispatch_.createDevice =
                    LoadInstanceFunction<PFN_vkCreateDevice>(dispatch_.getInstanceProcAddr, instance_, "vkCreateDevice");
                dispatch_.getDeviceProcAddr =
                    LoadInstanceFunction<PFN_vkGetDeviceProcAddr>(dispatch_.getInstanceProcAddr, instance_, "vkGetDeviceProcAddr");
                if (dispatch_.destroyInstance == nullptr || dispatch_.enumeratePhysicalDevices == nullptr ||
                    dispatch_.getPhysicalDeviceProperties2 == nullptr || dispatch_.getPhysicalDeviceFeatures2 == nullptr ||
                    dispatch_.getPhysicalDeviceMemoryProperties == nullptr || dispatch_.enumerateDeviceExtensions == nullptr ||
                    dispatch_.getQueueFamilyProperties == nullptr || dispatch_.createDevice == nullptr ||
                    dispatch_.getDeviceProcAddr == nullptr) {
                    return Result<void>::Failure(MissingEntryPoint("required Vulkan 1.3 instance dispatch"));
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<VulkanAdapterCandidate> InspectAdapter(const VkPhysicalDevice device) {
                VkPhysicalDeviceIDProperties identity{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &identity};
                dispatch_.getPhysicalDeviceProperties2(device, &properties);

                auto extensions = EnumerateDeviceExtensions(device);
                if (extensions.HasError())
                    return Result<VulkanAdapterCandidate>::Failure(extensions.ErrorValue());
                const bool hasMaintenance =
                    std::ranges::find(extensions.Value(), VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) != extensions.Value().end();
                VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
                VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                            .pNext = hasMaintenance ? &maintenance : nullptr};
                VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                            .pNext = &features13};
                VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
                dispatch_.getPhysicalDeviceFeatures2(device, &features);

                auto queues = EnumerateQueues(device);
                if (queues.HasError()) {
                    return Result<VulkanAdapterCandidate>::Failure(std::move(queues).ErrorValue());
                }
                const auto hostAllowed = loaderPort_->AcceptPhysicalDevice(instance_, device);
                if (hostAllowed.HasError())
                    return Result<VulkanAdapterCandidate>::Failure(hostAllowed.ErrorValue());
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
                    .hostAllowed = hostAllowed.Value(),
                    .vertexLayerRouting = features12.shaderOutputLayer == VK_TRUE,
                    .swapchainMaintenance = hasMaintenance && maintenance.swapchainMaintenance1 == VK_TRUE,
                });
            }

            [[nodiscard]] std::uint64_t DedicatedVideoMemory(const VkPhysicalDevice device, const VkPhysicalDeviceType type) const {
                if (type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    return 0;
                }
                VkPhysicalDeviceMemoryProperties properties{};
                dispatch_.getPhysicalDeviceMemoryProperties(device, &properties);
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
                    return dispatch_.enumerateDeviceExtensions(device, nullptr, count, properties);
                }, [](const VkExtensionProperties &property) {
                    return std::string{property.extensionName};
                }, "vkEnumerateDeviceExtensionProperties");
            }

            [[nodiscard]] Result<std::vector<VulkanQueueFamily>> EnumerateQueues(const VkPhysicalDevice device) const {
                std::uint32_t count{0};
                dispatch_.getQueueFamilyProperties(device, &count, nullptr);
                if (count > MaxQueueFamilies) {
                    return Result<std::vector<VulkanQueueFamily>>::Failure(MakeError(VulkanBackendErrors::EnumerationLimitExceeded));
                }
                std::vector<VkQueueFamilyProperties> properties(count);
                dispatch_.getQueueFamilyProperties(device, &count, properties.data());
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
                        .supportsTransfer =
                            properties[index].queueCount > 0 &&
                            (properties[index].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0,
                    });
                }
                return Result<std::vector<VulkanQueueFamily>>::Success(std::move(queues));
            }

            [[nodiscard]] Result<void> LoadDeviceDispatch(const VulkanDeviceRequest &request) {
                dispatch_.destroyDevice = LoadDeviceFunction<PFN_vkDestroyDevice>(dispatch_.getDeviceProcAddr, device_, "vkDestroyDevice");
                const auto getDeviceQueue =
                    LoadDeviceFunction<PFN_vkGetDeviceQueue>(dispatch_.getDeviceProcAddr, device_, "vkGetDeviceQueue");
                if (dispatch_.destroyDevice == nullptr || getDeviceQueue == nullptr) {
                    return Result<void>::Failure(MissingEntryPoint("required Vulkan device dispatch"));
                }
                getDeviceQueue(device_, request.graphicsQueueFamily, 0, &graphicsQueue_);
                getDeviceQueue(device_, request.presentationQueueFamily, 0, &presentationQueue_);
                getDeviceQueue(device_, request.transferQueueFamily, 0, &transferQueue_);
                if (graphicsQueue_ == VK_NULL_HANDLE || presentationQueue_ == VK_NULL_HANDLE || transferQueue_ == VK_NULL_HANDLE) {
                    return Result<void>::Failure(
                        MakeError(VulkanBackendErrors::NativeCallFailed, "Vulkan returned a null required queue."));
                }
                return Result<void>::Success();
            }

            void ClearInstanceDispatch() noexcept {
                dispatch_.destroyInstance = nullptr;
                dispatch_.enumeratePhysicalDevices = nullptr;
                dispatch_.getPhysicalDeviceProperties2 = nullptr;
                dispatch_.getPhysicalDeviceFeatures2 = nullptr;
                dispatch_.getPhysicalDeviceMemoryProperties = nullptr;
                dispatch_.enumerateDeviceExtensions = nullptr;
                dispatch_.getQueueFamilyProperties = nullptr;
                dispatch_.createDevice = nullptr;
                dispatch_.getDeviceProcAddr = nullptr;
            }

            /** @brief Native dispatch belongs to this exact loader/instance/device lifetime. */
            struct Dispatch {
                PFN_vkGetInstanceProcAddr getInstanceProcAddr{nullptr};
                PFN_vkCreateInstance createInstance{nullptr};
                PFN_vkDestroyInstance destroyInstance{nullptr};
                PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices{nullptr};
                PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2{nullptr};
                PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2{nullptr};
                PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties{nullptr};
                PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensions{nullptr};
                PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilyProperties{nullptr};
                PFN_vkCreateDevice createDevice{nullptr};
                PFN_vkGetDeviceProcAddr getDeviceProcAddr{nullptr};
                PFN_vkDestroyDevice destroyDevice{nullptr};
            };

            Dispatch dispatch_;

            IVulkanLoaderPort *loaderPort_{nullptr};
            bool loaderAcquired_{false};
            const std::thread::id editorOwner_{std::this_thread::get_id()};
            Detail::VulkanEditorGraphicsBridge *editorBridge_{};
            std::array<Detail::IVulkanEditorDeviceChild *, 2> editorChildren_{};
            VkPhysicalDevice editorPhysical_{};
            std::uint32_t editorGraphicsFamily_{};
            std::uint32_t editorFrames_{};
            RenderQueueId editorGraphicsIdentity_;
            Detail::VulkanPresentationFrame presentationFrame_;
            FrameToken presentationFrameToken_;
            RenderTimelinePoint presentationCompletion_;
            PrimaryOutputAttachment presentationOutput_;
            bool primaryOutputPrepared_{};

            struct EnabledGraphicsState {
                std::optional<VulkanHostGraphicsRequirements> host;
                VulkanApiVersion api;
                std::vector<std::string> instanceExtensions;
                std::vector<std::string> deviceExtensions;
            };

            EnabledGraphicsState enabledGraphics_;

            VkInstance instance_{VK_NULL_HANDLE};

            std::vector<NativeAdapter> nativeAdapters_;
            VkDevice device_{VK_NULL_HANDLE};
            VkQueue graphicsQueue_{VK_NULL_HANDLE};
            VkQueue presentationQueue_{VK_NULL_HANDLE};
            VkSurfaceKHR surface_{VK_NULL_HANDLE};
            VkQueue transferQueue_{VK_NULL_HANDLE};
            std::uint64_t nextFrame_{1};

            RenderCapabilitySnapshot resourceCapabilities_;
            Detail::VulkanResourceRuntime resources_;
            Detail::VulkanGraphRuntime graph_;
            std::function<Result<bool>(RenderTimelinePoint)> observePresentation_;
            std::unique_ptr<Detail::VulkanPresentationRuntime> presentation_;
        };
    }  // namespace

    namespace Detail {
        /** @copydoc QueryMultiviewDepthLimits */
        Result<VulkanMultiviewDepthLimits> QueryMultiviewDepthLimits(const VkPhysicalDevice device, const VulkanDepthFormatQuery &query,
                                                                     const std::uint32_t implementedViews,
                                                                     const VkPhysicalDeviceLimits &limits) {
            if (implementedViews == 0)
                return Result<VulkanMultiviewDepthLimits>::Success({});
            if (!query)
                return Result<VulkanMultiviewDepthLimits>::Failure(MissingEntryPoint("vkGetPhysicalDeviceImageFormatProperties"));
            VkImageFormatProperties format{};
            const auto result = query(device, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &format);
            if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
                return Result<VulkanMultiviewDepthLimits>::Success({});
            if (result != VK_SUCCESS)
                return Result<VulkanMultiviewDepthLimits>::Failure(NativeFailure("vkGetPhysicalDeviceImageFormatProperties", result));
            if (format.maxMipLevels == 0 || format.maxExtent.depth == 0 || (format.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0)
                return Result<VulkanMultiviewDepthLimits>::Success({});
            const auto views =
                std::min({implementedViews, 32U, limits.maxFramebufferLayers, limits.maxImageArrayLayers, format.maxArrayLayers});
            const auto extent = std::min({limits.maxImageDimension2D, format.maxExtent.width, format.maxExtent.height});
            return Result<VulkanMultiviewDepthLimits>::Success(views == 0 || extent == 0 ? VulkanMultiviewDepthLimits{}
                                                                                         : VulkanMultiviewDepthLimits{views, extent});
        }

        std::shared_ptr<IVulkanRuntimePort> CreateVulkanNativeRuntime(IVulkanLoaderPort &loaderPort,
                                                                      VulkanEditorGraphicsBridge *editorBridge) {
            if (editorBridge && editorBridge->Port())
                return {};
            auto runtime = std::make_shared<VulkanNativeRuntime>(loaderPort, editorBridge);
            if (editorBridge && !editorBridge->Attach(*runtime))
                return {};
            return runtime;
        }
    }  // namespace Detail
}  // namespace Horo::Render
