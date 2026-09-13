#pragma once

/**
 * @file VulkanBackendModule.h
 * @brief Private Vulkan loader, adapter, device, and queue composition contracts.
 */

#include "Horo/Runtime/Render/RenderBackendRegistry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace Horo::Render {
    /**
     * @brief Platform-owned Vulkan loader and presentation attachment.
     *
     * The platform owns loader serialization and any surface needed to answer
     * presentation-support queries. The Vulkan backend owns instances, physical-
     * device enumeration, logical devices, dispatch tables, and queues. Calls are
     * serialized on the host-declared render-capable owner thread.
     */
    class IVulkanLoaderPort {
    public:
        virtual ~IVulkanLoaderPort() = default;

        /** @brief Acquires the one platform loader lease used by this backend. */
        [[nodiscard]] virtual Result<void> AcquireLoader() = 0;
        /** @brief Returns the loader's instance procedure resolver after acquisition. */
        [[nodiscard]] virtual PFN_vkGetInstanceProcAddr GetInstanceProcAddr() noexcept = 0;
        /** @brief Returns the exact platform-selected WSI instance extensions. */
        [[nodiscard]] virtual Result<std::vector<std::string>> RequiredPresentationExtensions() = 0;
        /** @brief Tests actual-surface presentation support for one native queue family. */
        [[nodiscard]] virtual Result<bool> SupportsPresentation(VkPhysicalDevice device, std::uint32_t queueFamily) = 0;
        /** @brief Applies the host's versioned driver-admission rules to one native device. */
        [[nodiscard]] virtual Result<bool> IsDriverAllowed(const VkPhysicalDeviceProperties &properties) = 0;
        /** @brief Releases the loader after all descendant Vulkan objects are destroyed. */
        virtual void ReleaseLoader() noexcept = 0;
    };

    /** @brief Vulkan API version reported independently by the loader and an adapter. */
    struct VulkanApiVersion {
        std::uint32_t major{0};
        std::uint32_t minor{0};
        std::uint32_t patch{0};

        [[nodiscard]] constexpr auto operator<=>(const VulkanApiVersion &) const noexcept = default;
    };

    /** @brief Required Vulkan 1.3 feature bits queried before logical-device creation. */
    struct VulkanRequiredFeatures {
        bool dynamicRendering{false};
        bool synchronization2{false};
        bool timelineSemaphore{false};

        /** @brief Reports whether every baseline feature is available. */
        [[nodiscard]] constexpr bool AreAvailable() const noexcept {
            return dynamicRendering && synchronization2 && timelineSemaphore;
        }
    };

    /** @brief Queue-family facts used for graphics and optional presentation selection. */
    struct VulkanQueueFamily {
        std::uint32_t index{0};
        bool supportsGraphics{false};
        bool supportsPresentation{false};
    };

    /** @brief Native-free Vulkan physical-device facts returned by the platform runtime seam. */
    struct VulkanAdapterCandidate {
        RenderAdapterId id;
        std::string displayName;
        RenderAdapterKind kind{RenderAdapterKind::Unknown};
        std::uint64_t dedicatedVideoMemoryBytes{0};
        VulkanApiVersion apiVersion;
        std::uint32_t driverVersion{0};
        VulkanRequiredFeatures requiredFeatures;
        std::vector<std::string> deviceExtensions;
        std::vector<VulkanQueueFamily> queueFamilies;
        bool standardApiVariant{true};
        bool portabilitySubset{false};
        bool driverAllowed{true};
    };

    /** @brief One bounded physical-device enumeration and its non-zero revision. */
    struct VulkanAdapterEnumeration {
        std::uint64_t revision{0};
        std::vector<VulkanAdapterCandidate> adapters;
    };

    /** @brief Loader and instance facts queried before creating a Vulkan instance. */
    struct VulkanLoaderCapabilities {
        VulkanApiVersion apiVersion;
        std::vector<std::string> instanceExtensions;
        std::vector<std::string> layers;
        bool hasInstanceVersionQuery{false};
        bool standardApiVariant{true};
    };

    /** @brief Exact instance creation request derived from host and platform requirements. */
    struct VulkanInstanceRequest {
        VulkanApiVersion apiVersion;
        std::vector<std::string> enabledExtensions;
        bool enableValidation{false};
    };

    /** @brief Exact selected queue and feature request for logical-device creation. */
    struct VulkanDeviceRequest {
        RenderAdapterId adapter;
        std::uint32_t graphicsQueueFamily{0};
        std::uint32_t presentationQueueFamily{0};
        VulkanRequiredFeatures enabledFeatures;
        std::vector<std::string> enabledExtensions;
    };

    /**
     * @brief Backend-owned native Vulkan runtime seam used for policy tests.
     *
     * The production implementation acquires the platform loader and owns
     * instance/device dispatch. Tests replace it with deterministic native-free
     * behavior. Every method is invoked
     * synchronously on one render-capable owner thread. Release methods are
     * idempotent and must tolerate partial initialization.
     */
    class IVulkanRuntimePort {
    public:
        virtual ~IVulkanRuntimePort() = default;

        /** @brief Acquires one host loader lease without creating an instance. */
        [[nodiscard]] virtual Result<void> AcquireLoader() = 0;
        /** @brief Queries loader entry points, version, extensions, and layers. */
        [[nodiscard]] virtual Result<VulkanLoaderCapabilities> QueryLoaderCapabilities() = 0;
        /** @brief Returns platform-selected WSI instance extensions, empty for headless use. */
        [[nodiscard]] virtual Result<std::vector<std::string>> RequiredPresentationExtensions() = 0;
        /** @brief Creates the instance and its instance-owned dispatch table. */
        [[nodiscard]] virtual Result<void> CreateInstance(const VulkanInstanceRequest &request) = 0;
        /** @brief Enumerates at most maxAdapters physical-device records. */
        [[nodiscard]] virtual Result<VulkanAdapterEnumeration> EnumeratePhysicalDevices(std::uint32_t maxAdapters) = 0;
        /** @brief Creates the logical device, device dispatch, and requested queues. */
        [[nodiscard]] virtual Result<void> CreateDevice(const VulkanDeviceRequest &request) = 0;
        /** @brief Releases device queues, device dispatch, and the logical device. */
        virtual void DestroyDevice() noexcept = 0;
        /** @brief Releases instance dispatch and the instance. */
        virtual void DestroyInstance() noexcept = 0;
        /** @brief Releases the loader lease after every descendant object is gone. */
        virtual void ReleaseLoader() noexcept = 0;
    };

    /** @brief Returns inert native-free module metadata for host window planning. */
    [[nodiscard]] const RenderBackendModuleInfo &GetVulkanRenderBackendModuleInfo() noexcept;

    /**
     * @brief Creates an inert bounded Vulkan adapter-discovery service.
     * @param runtimePort Borrowed platform runtime port that outlives the service.
     * @return Owned discovery service; native acquisition occurs only in Discover.
     */
    [[nodiscard]] std::unique_ptr<IRenderAdapterDiscovery> CreateVulkanAdapterDiscovery(IVulkanLoaderPort &loaderPort);

    /**
     * @brief Registers the Vulkan backend using a host-owned loader/runtime port.
     * @param registry Host composition registry.
     * @param loaderPort Borrowed platform loader port that outlives the registry and all created backends.
     * @return Registration result; no loader, instance, device, or queue is acquired.
     */
    [[nodiscard]] Result<void> RegisterVulkanRenderBackend(RenderBackendRegistry &registry, IVulkanLoaderPort &loaderPort);
}  // namespace Horo::Render
