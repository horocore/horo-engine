#pragma once

#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanResourceRuntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <type_traits>

namespace Horo::Render::VulkanResourceTest {
    template <typename Handle> [[nodiscard]] Handle NativeHandle(const std::uintptr_t value) noexcept {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<Handle>(value);
        else
            return static_cast<Handle>(value);
    }

    template <typename Function> [[nodiscard]] PFN_vkVoidFunction EraseFunction(Function function) noexcept {
        static_assert(sizeof(Function) == sizeof(PFN_vkVoidFunction));
        return std::bit_cast<PFN_vkVoidFunction>(function);
    }

    struct NativeResourceState {
        VkMemoryRequirements bufferRequirements{.size = 64, .alignment = 16, .memoryTypeBits = 1};
        VkMemoryRequirements imageRequirements{.size = 256, .alignment = 16, .memoryTypeBits = 1};
        std::array<std::byte, 4096> mapped{};
        int createBufferCount{0};
        int destroyBufferCount{0};
        int createImageCount{0};
        int destroyImageCount{0};
        int createViewCount{0};
        int destroyViewCount{0};
        int allocateCount{0};
        int freeCount{0};
        VkResult createBufferResult{VK_SUCCESS};
        VkResult createImageResult{VK_SUCCESS};
        VkResult allocateResult{VK_SUCCESS};
        VkResult bindBufferResult{VK_SUCCESS};
        VkResult bindImageResult{VK_SUCCESS};
        VkResult mapResult{VK_SUCCESS};
        VkResult createViewResult{VK_SUCCESS};
    };

    inline NativeResourceState *nativeResources{nullptr};

    struct NativeResourceScope {
        NativeResourceScope() noexcept {
            nativeResources = &state;
        }

        ~NativeResourceScope() {
            nativeResources = nullptr;
        }

        NativeResourceState state;
    };

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *,
                                                           VkBuffer *buffer) {
        ++nativeResources->createBufferCount;
        if (nativeResources->createBufferResult != VK_SUCCESS)
            return nativeResources->createBufferResult;
        *buffer = NativeHandle<VkBuffer>(10 + nativeResources->createBufferCount);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *) {
        ++nativeResources->destroyBufferCount;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *requirements) {
        *requirements = nativeResources->bufferRequirements;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *,
                                                          VkImage *image) {
        ++nativeResources->createImageCount;
        if (nativeResources->createImageResult != VK_SUCCESS)
            return nativeResources->createImageResult;
        *image = NativeHandle<VkImage>(12);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *) {
        ++nativeResources->destroyImageCount;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements *requirements) {
        *requirements = nativeResources->imageRequirements;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *,
                                                             VkDeviceMemory *memory) {
        ++nativeResources->allocateCount;
        if (nativeResources->allocateResult != VK_SUCCESS)
            return nativeResources->allocateResult;
        *memory = NativeHandle<VkDeviceMemory>(13);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {
        ++nativeResources->freeCount;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
        return nativeResources->bindBufferResult;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
        return nativeResources->bindImageResult;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags,
                                                        void **data) {
        if (nativeResources->mapResult != VK_SUCCESS)
            return nativeResources->mapResult;
        *data = nativeResources->mapped.data();
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeUnmapMemory(VkDevice, VkDeviceMemory) {}

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImageView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *,
                                                              VkImageView *view) {
        ++nativeResources->createViewCount;
        if (nativeResources->createViewResult != VK_SUCCESS)
            return nativeResources->createViewResult;
        *view = NativeHandle<VkImageView>(14);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *) {
        ++nativeResources->destroyViewCount;
    }

    [[nodiscard]] inline PFN_vkVoidFunction VKAPI_CALL MissingResourceResolver(VkDevice, const char *) {
        return nullptr;
    }

    [[nodiscard]] inline PFN_vkVoidFunction VKAPI_CALL FakeResourceResolver(VkDevice, const char *name) {
        struct Entry {
            std::string_view name;
            PFN_vkVoidFunction function;
        };

        const std::array entries{
            Entry{"vkCreateBuffer", EraseFunction(PFN_vkCreateBuffer{FakeCreateBuffer})},
            Entry{"vkDestroyBuffer", EraseFunction(PFN_vkDestroyBuffer{FakeDestroyBuffer})},
            Entry{"vkGetBufferMemoryRequirements", EraseFunction(PFN_vkGetBufferMemoryRequirements{FakeGetBufferMemoryRequirements})},
            Entry{"vkCreateImage", EraseFunction(PFN_vkCreateImage{FakeCreateImage})},
            Entry{"vkDestroyImage", EraseFunction(PFN_vkDestroyImage{FakeDestroyImage})},
            Entry{"vkGetImageMemoryRequirements", EraseFunction(PFN_vkGetImageMemoryRequirements{FakeGetImageMemoryRequirements})},
            Entry{"vkAllocateMemory", EraseFunction(PFN_vkAllocateMemory{FakeAllocateMemory})},
            Entry{"vkFreeMemory", EraseFunction(PFN_vkFreeMemory{FakeFreeMemory})},
            Entry{"vkBindBufferMemory", EraseFunction(PFN_vkBindBufferMemory{FakeBindBufferMemory})},
            Entry{"vkBindImageMemory", EraseFunction(PFN_vkBindImageMemory{FakeBindImageMemory})},
            Entry{"vkMapMemory", EraseFunction(PFN_vkMapMemory{FakeMapMemory})},
            Entry{"vkUnmapMemory", EraseFunction(PFN_vkUnmapMemory{FakeUnmapMemory})},
            Entry{"vkCreateImageView", EraseFunction(PFN_vkCreateImageView{FakeCreateImageView})},
            Entry{"vkDestroyImageView", EraseFunction(PFN_vkDestroyImageView{FakeDestroyImageView})},
        };
        const auto found = std::ranges::find(entries, std::string_view{name}, &Entry::name);
        return found == entries.end() ? nullptr : found->function;
    }

    inline void InitializeNativeResourceRuntime(Detail::VulkanResourceRuntime &runtime) {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        memoryProperties.memoryTypeCount = 1;
        memoryProperties.memoryTypes[0].propertyFlags =
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        REQUIRE(runtime.Initialize(memoryProperties, NativeHandle<VkDevice>(1), FakeResourceResolver).HasValue());
    }

    [[nodiscard]] inline RenderBufferDescriptor HostVisibleVertexBuffer() noexcept {
        return {.byteSize = 64, .usage = RenderBufferUsage::Vertex, .access = RenderBufferAccess::HostVisible};
    }

}  // namespace Horo::Render::VulkanResourceTest
