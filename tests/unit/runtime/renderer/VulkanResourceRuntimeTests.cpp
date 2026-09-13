#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanResourceRuntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <type_traits>

namespace {
    using namespace Horo::Render;

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
        std::array<std::byte, 256> mapped{};
        int createBufferCount{0};
        int destroyBufferCount{0};
        int createImageCount{0};
        int destroyImageCount{0};
        int createViewCount{0};
        int destroyViewCount{0};
        int allocateCount{0};
        int freeCount{0};
    };

    NativeResourceState *nativeResources{nullptr};

    struct NativeResourceScope {
        NativeResourceScope() noexcept {
            nativeResources = &state;
        }

        ~NativeResourceScope() {
            nativeResources = nullptr;
        }

        NativeResourceState state;
    };

    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *buffer) {
        ++nativeResources->createBufferCount;
        *buffer = NativeHandle<VkBuffer>(11);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL FakeDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *) {
        ++nativeResources->destroyBufferCount;
    }

    VKAPI_ATTR void VKAPI_CALL FakeGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *requirements) {
        *requirements = nativeResources->bufferRequirements;
    }

    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *image) {
        ++nativeResources->createImageCount;
        *image = NativeHandle<VkImage>(12);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL FakeDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *) {
        ++nativeResources->destroyImageCount;
    }

    VKAPI_ATTR void VKAPI_CALL FakeGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements *requirements) {
        *requirements = nativeResources->imageRequirements;
    }

    VKAPI_ATTR VkResult VKAPI_CALL FakeAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *,
                                                      VkDeviceMemory *memory) {
        ++nativeResources->allocateCount;
        *memory = NativeHandle<VkDeviceMemory>(13);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL FakeFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {
        ++nativeResources->freeCount;
    }

    VKAPI_ATTR VkResult VKAPI_CALL FakeBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL FakeBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL FakeMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **data) {
        *data = nativeResources->mapped.data();
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL FakeUnmapMemory(VkDevice, VkDeviceMemory) {}

    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImageView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *,
                                                       VkImageView *view) {
        ++nativeResources->createViewCount;
        *view = NativeHandle<VkImageView>(14);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL FakeDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *) {
        ++nativeResources->destroyViewCount;
    }

    [[nodiscard]] PFN_vkVoidFunction VKAPI_CALL MissingResourceResolver(VkDevice, const char *) {
        return nullptr;
    }

    [[nodiscard]] PFN_vkVoidFunction VKAPI_CALL FakeResourceResolver(VkDevice, const char *name) {
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

    void InitializeNativeResourceRuntime(Detail::VulkanResourceRuntime &runtime) {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        memoryProperties.memoryTypeCount = 1;
        memoryProperties.memoryTypes[0].propertyFlags =
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        REQUIRE(runtime.Initialize(memoryProperties, NativeHandle<VkDevice>(1), FakeResourceResolver).HasValue());
    }

    TEST_CASE("Vulkan native buffer realization preserves accounting and non-reused identities", "[unit][runtime][renderer][vulkan]") {
        NativeResourceScope native;
        Detail::VulkanResourceRuntime runtime;
        InitializeNativeResourceRuntime(runtime);
        const RenderBufferDescriptor descriptor{.byteSize = 64,
                                                .usage = RenderBufferUsage::Vertex | RenderBufferUsage::CopyDestination,
                                                .access = RenderBufferAccess::HostVisible};
        std::array<std::byte, 64> initialData{};
        initialData.fill(std::byte{0x5A});

        const auto plan = runtime.QueryBufferMemoryCost(descriptor);
        REQUIRE(plan.HasValue());
        REQUIRE(plan.Value().IsValid());
        REQUIRE(plan.Value().requiredBytes == 64);
        REQUIRE(plan.Value().alignment == 16);
        const auto first = runtime.CreateBuffer(descriptor, initialData, TestSupport::PlacementFor(plan.Value(), 1));
        REQUIRE(first.HasValue());
        REQUIRE(std::ranges::equal(std::span{native.state.mapped}.first<64>(), initialData));

        runtime.DestroyBuffer(first.Value());
        const auto second = runtime.CreateBuffer(descriptor, initialData, TestSupport::PlacementFor(plan.Value(), 2));
        REQUIRE(second.HasValue());
        REQUIRE(second.Value() != first.Value());
        const int destroysBeforeStaleIdentity = native.state.destroyBufferCount;
        runtime.DestroyBuffer(first.Value());
        REQUIRE(native.state.destroyBufferCount == destroysBeforeStaleIdentity);
        runtime.DestroyBuffer(second.Value());
        REQUIRE(native.state.allocateCount == native.state.freeCount);
    }

    TEST_CASE("Vulkan native requirement probes reject malformed driver accounting", "[unit][runtime][renderer][vulkan]") {
        NativeResourceScope native;
        native.state.bufferRequirements.alignment = 3;
        Detail::VulkanResourceRuntime runtime;
        InitializeNativeResourceRuntime(runtime);
        const RenderBufferDescriptor descriptor{.byteSize = 64,
                                                .usage = RenderBufferUsage::Vertex,
                                                .access = RenderBufferAccess::HostVisible};

        const auto plan = runtime.QueryBufferMemoryCost(descriptor);
        REQUIRE(plan.HasError());
        REQUIRE(plan.ErrorValue().code.Value() == "render.vulkan.resource_creation_failed");
        REQUIRE(native.state.createBufferCount == 1);
        REQUIRE(native.state.destroyBufferCount == 1);
    }

    TEST_CASE("Vulkan resource initialization failure leaves the runtime reusable", "[unit][runtime][renderer][vulkan]") {
        NativeResourceScope native;
        Detail::VulkanResourceRuntime runtime;
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        memoryProperties.memoryTypeCount = 1;
        memoryProperties.memoryTypes[0].propertyFlags =
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        const auto failed = runtime.Initialize(memoryProperties, NativeHandle<VkDevice>(1), MissingResourceResolver);
        REQUIRE(failed.HasError());
        REQUIRE(failed.ErrorValue().code.Value() == "render.vulkan.entry_point_missing");
        REQUIRE(runtime.QueryBufferMemoryCost({.byteSize = 64, .usage = RenderBufferUsage::Vertex}).HasError());
        REQUIRE(runtime.Initialize(memoryProperties, NativeHandle<VkDevice>(1), FakeResourceResolver).HasValue());
    }

    TEST_CASE("Vulkan textures, views, and targets validate dependencies and retire deterministically",
              "[unit][runtime][renderer][vulkan]") {
        NativeResourceScope native;
        Detail::VulkanResourceRuntime runtime;
        InitializeNativeResourceRuntime(runtime);
        const RenderTextureDescriptor colorDescriptor{.extent = {8, 8},
                                                      .format = RenderTextureFormat::Rgba8Unorm,
                                                      .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
        const RenderTextureDescriptor depthDescriptor{.extent = {8, 8},
                                                      .format = RenderTextureFormat::Depth32Float,
                                                      .usage = RenderTextureUsage::RenderAttachment};
        const auto colorPlan = runtime.QueryTextureMemoryCost(colorDescriptor);
        const auto depthPlan = runtime.QueryTextureMemoryCost(depthDescriptor);
        REQUIRE(colorPlan.HasValue());
        REQUIRE(depthPlan.HasValue());
        const auto color = runtime.CreateTexture(colorDescriptor, {}, TestSupport::PlacementFor(colorPlan.Value(), 1));
        const auto depth = runtime.CreateTexture(depthDescriptor, {}, TestSupport::PlacementFor(depthPlan.Value(), 2));
        REQUIRE(color.HasValue());
        REQUIRE(depth.HasValue());

        const RenderTextureViewDescriptor colorViewDescriptor{.texture = {{1}, 1, 1},
                                                              .format = RenderTextureFormat::Rgba8Unorm,
                                                              .aspect = RenderTextureAspect::Color};
        const RenderTextureViewDescriptor depthViewDescriptor{.texture = {{1}, 2, 1},
                                                              .format = RenderTextureFormat::Depth32Float,
                                                              .aspect = RenderTextureAspect::Depth};
        const auto colorView = runtime.CreateTextureView(colorViewDescriptor, color.Value());
        const auto depthView = runtime.CreateTextureView(depthViewDescriptor, depth.Value());
        REQUIRE(colorView.HasValue());
        REQUIRE(depthView.HasValue());
        auto invalidRange = colorViewDescriptor;
        invalidRange.baseMip = 1;
        REQUIRE(runtime.CreateTextureView(invalidRange, color.Value()).HasError());

        const RenderTargetDescriptor targetDescriptor{.colorAttachment = {{1}, 1, 1}, .depthAttachment = {{1}, 2, 1}, .extent = {8, 8}};
        REQUIRE(runtime.CreateRenderTarget(targetDescriptor, colorView.Value(), depthView.Value()).HasValue());
        const std::array<std::byte, 4> unsupportedUpload{};
        const auto uploaded = runtime.CreateTexture(colorDescriptor, unsupportedUpload, TestSupport::PlacementFor(colorPlan.Value(), 3));
        REQUIRE(uploaded.HasError());
        REQUIRE(uploaded.ErrorValue().code.Value() == "render.vulkan.resource_unsupported");

        const std::array destroyedBeforeRetirement{native.state.destroyImageCount, native.state.destroyViewCount};
        runtime.DestroyTexture(depth.Value());
        REQUIRE(native.state.destroyImageCount == destroyedBeforeRetirement[0] + 1);
        REQUIRE(native.state.destroyViewCount == destroyedBeforeRetirement[1] + 1);
        REQUIRE(runtime.CreateRenderTarget(targetDescriptor, colorView.Value(), depthView.Value()).HasError());

        runtime.ShutdownResources();
        runtime.ShutdownResources();
        REQUIRE(native.state.createImageCount == native.state.destroyImageCount);
        REQUIRE(native.state.createViewCount == native.state.destroyViewCount);
        REQUIRE(native.state.allocateCount == native.state.freeCount);
        REQUIRE(runtime.QueryTextureMemoryCost(colorDescriptor).HasError());
    }
}  // namespace
