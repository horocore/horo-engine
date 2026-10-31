#pragma once
#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanResourceConversions.h"
#include "runtime/renderer/modules/vulkan/VulkanResourceRuntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

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
        bool sizedAllocations{}; /**< Opt-in full viewport fixture: retain exact native allocation byte bounds. */
        std::map<VkBuffer, VkDeviceSize> bufferSizes;
        std::map<VkImage, VkDeviceSize> imageSizes;
        std::map<VkDeviceMemory, std::vector<std::byte>> allocations;
        int createBufferCount{0};
        int destroyBufferCount{0};
        int createImageCount{0};
        int destroyImageCount{0};
        int createViewCount{0};
        int failCreateViewCall{0}; /**< One exact scripted realization boundary; zero leaves existing failures unchanged. */
        VkImageAspectFlags lastViewAspectMask{};
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

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeCreateBuffer(VkDevice, const VkBufferCreateInfo *info, const VkAllocationCallbacks *,
                                                           VkBuffer *buffer) {
        ++nativeResources->createBufferCount;
        if (nativeResources->createBufferResult != VK_SUCCESS)
            return nativeResources->createBufferResult;
        *buffer = NativeHandle<VkBuffer>(10 + nativeResources->createBufferCount);
        if (nativeResources->sizedAllocations)
            nativeResources->bufferSizes.emplace(*buffer, info->size);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks *) {
        ++nativeResources->destroyBufferCount;
        nativeResources->bufferSizes.erase(buffer);
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeGetBufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements *requirements) {
        *requirements = nativeResources->bufferRequirements;
        if (nativeResources->sizedAllocations) {
            const auto bytes = nativeResources->bufferSizes.at(buffer);
            requirements->size =
                std::max(requirements->size, (bytes + requirements->alignment - 1) / requirements->alignment * requirements->alignment);
        }
    }

    /** @brief Uses production format conversion and checked payload sizing; it makes no capability or raster claim. */
    [[nodiscard]] inline std::optional<VkDeviceSize> CapturedImageBytes(const VkImageCreateInfo &info) {
        if (info.imageType != VK_IMAGE_TYPE_2D || info.mipLevels != 1 || info.format == VK_FORMAT_UNDEFINED)
            return std::nullopt;
        for (std::uint32_t value = 0; value <= static_cast<std::uint32_t>(RenderTextureFormat::Depth32FloatStencil8); ++value) {
            const auto format = static_cast<RenderTextureFormat>(value);
            if (Detail::VulkanResourceConversion::TextureFormat(format) != info.format)
                continue;
            const RenderTextureDescriptor descriptor{.extent = {info.extent.width, info.extent.height},
                                                     .format = format,
                                                     .layerCount = info.arrayLayers,
                                                     .sampleCount = static_cast<std::uint32_t>(info.samples),
                                                     .usage = RenderTextureUsage::RenderAttachment,
                                                     .depth = info.extent.depth};
            const auto bytes = RenderTextureBaseLevelByteSize(descriptor);
            return bytes.has_value() ? std::optional<VkDeviceSize>{*bytes} : std::nullopt;
        }
        return std::nullopt;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImage(VkDevice, const VkImageCreateInfo *info, const VkAllocationCallbacks *,
                                                          VkImage *image) {
        ++nativeResources->createImageCount;
        if (nativeResources->createImageResult != VK_SUCCESS)
            return nativeResources->createImageResult;
        if (nativeResources->sizedAllocations) {
            const auto bytes = info != nullptr ? CapturedImageBytes(*info) : std::nullopt;
            if (!bytes.has_value())
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            *image = NativeHandle<VkImage>(12 + nativeResources->createImageCount);
            nativeResources->imageSizes.emplace(*image, *bytes);
        } else {
            *image = NativeHandle<VkImage>(12);
        }
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeDestroyImage(VkDevice, VkImage image, const VkAllocationCallbacks *) {
        ++nativeResources->destroyImageCount;
        nativeResources->imageSizes.erase(image);
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeGetImageMemoryRequirements(VkDevice, VkImage image, VkMemoryRequirements *requirements) {
        *requirements = nativeResources->imageRequirements;
        if (nativeResources->sizedAllocations) {
            const auto bytes = nativeResources->imageSizes.at(image);
            const auto alignment = requirements->alignment;
            if (alignment == 0 || !std::has_single_bit(alignment) || bytes > (std::numeric_limits<VkDeviceSize>::max)() - (alignment - 1)) {
                requirements->size = 0;
                return;
            }
            requirements->size = std::max(requirements->size, (bytes + alignment - 1) / alignment * alignment);
        }
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeAllocateMemory(VkDevice, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *,
                                                             VkDeviceMemory *memory) {
        ++nativeResources->allocateCount;
        if (nativeResources->allocateResult != VK_SUCCESS)
            return nativeResources->allocateResult;
        *memory = NativeHandle<VkDeviceMemory>(nativeResources->sizedAllocations ? 1000 + nativeResources->allocateCount : 13);
        if (nativeResources->sizedAllocations)
            nativeResources->allocations.emplace(*memory, std::vector<std::byte>(info->allocationSize));
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks *) {
        ++nativeResources->freeCount;
        nativeResources->allocations.erase(memory);
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
        return nativeResources->bindBufferResult;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
        return nativeResources->bindImageResult;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                                        VkMemoryMapFlags, void **data) {
        if (nativeResources->mapResult != VK_SUCCESS)
            return nativeResources->mapResult;
        if (nativeResources->sizedAllocations) {
            auto &bytes = nativeResources->allocations.at(memory);
            REQUIRE(offset <= bytes.size());
            REQUIRE((size == VK_WHOLE_SIZE || size <= bytes.size() - offset));
            *data = bytes.data() + offset;
        } else {
            *data = nativeResources->mapped.data();
        }
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL FakeUnmapMemory(VkDevice, VkDeviceMemory) {}

    inline VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImageView(VkDevice, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *,
                                                              VkImageView *view) {
        ++nativeResources->createViewCount;
        nativeResources->lastViewAspectMask = info->subresourceRange.aspectMask;
        if (nativeResources->failCreateViewCall == nativeResources->createViewCount)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
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
