#include "../../../support/renderer/RenderGraphSubmissionFixture.h"
#include "RenderMemoryTestSupport.h"
#include "VulkanCommandTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanGraphRuntime.h"
#include "runtime/renderer/modules/vulkan/VulkanResourceConversions.h"
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
        VkResult createBufferResult{VK_SUCCESS};
        VkResult createImageResult{VK_SUCCESS};
        VkResult allocateResult{VK_SUCCESS};
        VkResult bindBufferResult{VK_SUCCESS};
        VkResult bindImageResult{VK_SUCCESS};
        VkResult mapResult{VK_SUCCESS};
        VkResult createViewResult{VK_SUCCESS};
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
        if (nativeResources->createBufferResult != VK_SUCCESS)
            return nativeResources->createBufferResult;
        *buffer = NativeHandle<VkBuffer>(10 + nativeResources->createBufferCount);
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
        if (nativeResources->createImageResult != VK_SUCCESS)
            return nativeResources->createImageResult;
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
        if (nativeResources->allocateResult != VK_SUCCESS)
            return nativeResources->allocateResult;
        *memory = NativeHandle<VkDeviceMemory>(13);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL FakeFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {
        ++nativeResources->freeCount;
    }

    VKAPI_ATTR VkResult VKAPI_CALL FakeBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
        return nativeResources->bindBufferResult;
    }

    VKAPI_ATTR VkResult VKAPI_CALL FakeBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
        return nativeResources->bindImageResult;
    }

    VKAPI_ATTR VkResult VKAPI_CALL FakeMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **data) {
        if (nativeResources->mapResult != VK_SUCCESS)
            return nativeResources->mapResult;
        *data = nativeResources->mapped.data();
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL FakeUnmapMemory(VkDevice, VkDeviceMemory) {}

    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImageView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *,
                                                       VkImageView *view) {
        ++nativeResources->createViewCount;
        if (nativeResources->createViewResult != VK_SUCCESS)
            return nativeResources->createViewResult;
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

    [[nodiscard]] RenderBufferDescriptor HostVisibleVertexBuffer() noexcept {
        return {.byteSize = 64, .usage = RenderBufferUsage::Vertex, .access = RenderBufferAccess::HostVisible};
    }

    TEST_CASE("Vulkan native buffer realization preserves accounting and non-reused identities", "[unit][runtime][renderer][vulkan]") {
        NativeResourceScope native;
        Detail::VulkanResourceRuntime runtime;
        InitializeNativeResourceRuntime(runtime);
        RenderBufferDescriptor descriptor = HostVisibleVertexBuffer();
        descriptor.usage = descriptor.usage | RenderBufferUsage::CopyDestination;
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
        const RenderBufferDescriptor descriptor = HostVisibleVertexBuffer();

        const auto plan = runtime.QueryBufferMemoryCost(descriptor);
        REQUIRE(plan.HasError());
        REQUIRE(plan.ErrorValue().code.Value() == "render.vulkan.resource_creation_failed");
        REQUIRE(native.state.createBufferCount == 1);
        REQUIRE(native.state.destroyBufferCount == 1);
    }

    TEST_CASE("Vulkan resource identities fail closed at the configured capacity", "[unit][runtime][renderer][vulkan]") {
        NativeResourceScope native;
        Detail::VulkanResourceRuntime runtime{1};
        InitializeNativeResourceRuntime(runtime);
        const RenderBufferDescriptor descriptor = HostVisibleVertexBuffer();
        const auto plan = runtime.QueryBufferMemoryCost(descriptor);
        REQUIRE(plan.HasValue());

        const auto first = runtime.CreateBuffer(descriptor, {}, TestSupport::PlacementFor(plan.Value(), 1));
        REQUIRE(first.HasValue());
        REQUIRE(first.Value() == 1);

        const auto exhausted = runtime.CreateBuffer(descriptor, {}, TestSupport::PlacementFor(plan.Value(), 2));
        REQUIRE(exhausted.HasError());
        REQUIRE(exhausted.ErrorValue().code.Value() == "render.vulkan.resource_creation_failed");
        REQUIRE(native.state.createBufferCount == native.state.destroyBufferCount + 1);

        runtime.DestroyBuffer(first.Value());
        REQUIRE(native.state.createBufferCount == native.state.destroyBufferCount);
        REQUIRE(native.state.allocateCount == native.state.freeCount);
    }

    TEST_CASE("Vulkan buffer and format conversions preserve backend-neutral policy", "[unit][runtime][renderer][vulkan]") {
        using namespace Detail::VulkanResourceConversion;

        const auto bufferUsage = BufferUsage(RenderBufferUsage::Vertex | RenderBufferUsage::Index | RenderBufferUsage::CopySource |
                                             RenderBufferUsage::CopyDestination | RenderBufferUsage::Uniform | RenderBufferUsage::Storage |
                                             RenderBufferUsage::Indirect);
        REQUIRE((bufferUsage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0);
        REQUIRE((bufferUsage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0);
        REQUIRE((bufferUsage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) != 0);
        REQUIRE((bufferUsage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0);
        REQUIRE((bufferUsage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0);
        REQUIRE((bufferUsage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0);
        REQUIRE((bufferUsage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) != 0);

        const std::array formatMappings{
            std::pair{RenderTextureFormat::Rgba8Unorm, VK_FORMAT_R8G8B8A8_UNORM},
            std::pair{RenderTextureFormat::Depth24Stencil8, VK_FORMAT_D24_UNORM_S8_UINT},
            std::pair{RenderTextureFormat::Depth32Float, VK_FORMAT_D32_SFLOAT},
            std::pair{RenderTextureFormat::R8Unorm, VK_FORMAT_R8_UNORM},
            std::pair{RenderTextureFormat::Rg8Unorm, VK_FORMAT_R8G8_UNORM},
            std::pair{RenderTextureFormat::Rgba8UnormSrgb, VK_FORMAT_R8G8B8A8_SRGB},
            std::pair{RenderTextureFormat::Bgra8Unorm, VK_FORMAT_B8G8R8A8_UNORM},
            std::pair{RenderTextureFormat::Bgra8UnormSrgb, VK_FORMAT_B8G8R8A8_SRGB},
            std::pair{RenderTextureFormat::R16Float, VK_FORMAT_R16_SFLOAT},
            std::pair{RenderTextureFormat::Rg16Float, VK_FORMAT_R16G16_SFLOAT},
            std::pair{RenderTextureFormat::Rgba16Float, VK_FORMAT_R16G16B16A16_SFLOAT},
            std::pair{RenderTextureFormat::R32Float, VK_FORMAT_R32_SFLOAT},
            std::pair{RenderTextureFormat::Rg32Float, VK_FORMAT_R32G32_SFLOAT},
            std::pair{RenderTextureFormat::Rgba32Float, VK_FORMAT_R32G32B32A32_SFLOAT},
            std::pair{RenderTextureFormat::Depth16Unorm, VK_FORMAT_D16_UNORM},
            std::pair{RenderTextureFormat::Depth32FloatStencil8, VK_FORMAT_D32_SFLOAT_S8_UINT},
        };
        for (const auto &[format, native] : formatMappings)
            REQUIRE(TextureFormat(format) == native);
        REQUIRE(TextureFormat(static_cast<RenderTextureFormat>(255)) == VK_FORMAT_UNDEFINED);
    }

    TEST_CASE("Vulkan image conversions preserve usage and dimensional policy", "[unit][runtime][renderer][vulkan]") {
        using namespace Detail::VulkanResourceConversion;
        RenderTextureDescriptor texture{.dimension = RenderTextureDimension::TwoD,
                                        .extent = {8, 8},
                                        .format = RenderTextureFormat::Rgba8Unorm,
                                        .mipCount = 2,
                                        .layerCount = 6,
                                        .sampleCount = 1,
                                        .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment |
                                                 RenderTextureUsage::CopySource | RenderTextureUsage::CopyDestination |
                                                 RenderTextureUsage::Storage,
                                        .depth = 1};
        const auto imageUsage = ImageUsage(texture);
        REQUIRE((imageUsage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
        REQUIRE((imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0);
        REQUIRE((imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0);
        REQUIRE((imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0);
        REQUIRE((imageUsage & VK_IMAGE_USAGE_STORAGE_BIT) != 0);
        REQUIRE(ImageFlags(texture) == VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
        const VkImageCreateInfo info = ImageCreateInfo(texture);
        REQUIRE(info.imageType == VK_IMAGE_TYPE_2D);
        REQUIRE(info.arrayLayers == 6);
        REQUIRE(info.mipLevels == 2);

        texture.format = RenderTextureFormat::Depth32Float;
        REQUIRE((ImageUsage(texture) & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
        texture.dimension = RenderTextureDimension::OneD;
        REQUIRE(ImageType(texture.dimension) == VK_IMAGE_TYPE_1D);
        REQUIRE(ImageFlags(texture) == 0);
        texture.dimension = RenderTextureDimension::ThreeD;
        REQUIRE(ImageType(texture.dimension) == VK_IMAGE_TYPE_3D);
        REQUIRE(ImageType(static_cast<RenderTextureDimension>(255)) == VK_IMAGE_TYPE_2D);
    }

    TEST_CASE("Vulkan view and memory conversions reject incompatible policy", "[unit][runtime][renderer][vulkan]") {
        using namespace Detail::VulkanResourceConversion;
        REQUIRE(ViewType(RenderTextureViewDimension::OneD) == VK_IMAGE_VIEW_TYPE_1D);
        REQUIRE(ViewType(RenderTextureViewDimension::TwoD) == VK_IMAGE_VIEW_TYPE_2D);
        REQUIRE(ViewType(RenderTextureViewDimension::TwoDArray) == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
        REQUIRE(ViewType(RenderTextureViewDimension::Cube) == VK_IMAGE_VIEW_TYPE_CUBE);
        REQUIRE(ViewType(RenderTextureViewDimension::CubeArray) == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
        REQUIRE(ViewType(RenderTextureViewDimension::ThreeD) == VK_IMAGE_VIEW_TYPE_3D);
        REQUIRE(ViewType(static_cast<RenderTextureViewDimension>(255)) == VK_IMAGE_VIEW_TYPE_2D);
        REQUIRE(Aspect(RenderTextureAspect::Color) == VK_IMAGE_ASPECT_COLOR_BIT);
        REQUIRE(Aspect(RenderTextureAspect::Depth) == VK_IMAGE_ASPECT_DEPTH_BIT);
        REQUIRE(Aspect(RenderTextureAspect::Stencil) == VK_IMAGE_ASPECT_STENCIL_BIT);
        REQUIRE(Aspect(RenderTextureAspect::DepthStencil) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));

        const VkMemoryRequirements requirements{.size = 256, .alignment = 16, .memoryTypeBits = 3};
        REQUIRE(RequirementsValid(requirements, 128));
        REQUIRE_FALSE(RequirementsValid({.size = 127, .alignment = 16, .memoryTypeBits = 3}, 128));
        REQUIRE_FALSE(RequirementsValid({.size = 256, .alignment = 3, .memoryTypeBits = 3}, 128));
        const RenderMemoryCostPlan plan{.memoryClass = RenderMemoryClass::PersistentDevice,
                                        .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                        .provenance = RenderMemoryCostProvenance::Exact,
                                        .compatibility = Compatibility(3, RenderMemoryClass::PersistentDevice),
                                        .payloadBytes = 128,
                                        .requiredBytes = 256,
                                        .alignment = 16};
        const auto placement = TestSupport::PlacementFor(plan, 1);
        REQUIRE(PlacementMatches(plan, placement));
        REQUIRE(RequirementsMatch(requirements, plan));
        auto mismatched = placement;
        mismatched.offsetBytes = 16;
        REQUIRE_FALSE(PlacementMatches(plan, mismatched));
        REQUIRE_FALSE(RequirementsMatch({.size = 512, .alignment = 16, .memoryTypeBits = 3}, plan));
    }

    TEST_CASE("Vulkan native failures roll back partial buffer and texture resources", "[unit][runtime][renderer][vulkan]") {
        NativeResourceScope native;
        Detail::VulkanResourceRuntime runtime;
        InitializeNativeResourceRuntime(runtime);
        const RenderBufferDescriptor bufferDescriptor{.byteSize = 64,
                                                      .usage = RenderBufferUsage::Vertex,
                                                      .access = RenderBufferAccess::HostVisible};
        const RenderTextureDescriptor textureDescriptor{.extent = {8, 8},
                                                        .format = RenderTextureFormat::Rgba8Unorm,
                                                        .usage = RenderTextureUsage::Sampled};
        const auto bufferPlan = runtime.QueryBufferMemoryCost(bufferDescriptor);
        const auto texturePlan = runtime.QueryTextureMemoryCost(textureDescriptor);
        REQUIRE(bufferPlan.HasValue());
        REQUIRE(texturePlan.HasValue());

        native.state.bindBufferResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        REQUIRE(runtime.CreateBuffer(bufferDescriptor, {}, TestSupport::PlacementFor(bufferPlan.Value(), 1)).HasError());
        native.state.bindBufferResult = VK_SUCCESS;
        native.state.mapResult = VK_ERROR_MEMORY_MAP_FAILED;
        std::array<std::byte, 64> bytes{};
        REQUIRE(runtime.CreateBuffer(bufferDescriptor, bytes, TestSupport::PlacementFor(bufferPlan.Value(), 2)).HasError());
        native.state.mapResult = VK_SUCCESS;

        native.state.bindImageResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        REQUIRE(runtime.CreateTexture(textureDescriptor, {}, TestSupport::PlacementFor(texturePlan.Value(), 3)).HasError());
        native.state.bindImageResult = VK_SUCCESS;
        native.state.createViewResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        const auto texture = runtime.CreateTexture(textureDescriptor, {}, TestSupport::PlacementFor(texturePlan.Value(), 4));
        REQUIRE(texture.HasValue());
        const RenderTextureViewDescriptor viewDescriptor{.texture = {{1}, 1, 1}, .format = RenderTextureFormat::Rgba8Unorm};
        REQUIRE(runtime.CreateTextureView(viewDescriptor, texture.Value()).HasError());

        runtime.ShutdownResources();
        REQUIRE(native.state.createBufferCount == native.state.destroyBufferCount);
        REQUIRE(native.state.createImageCount == native.state.destroyImageCount);
        REQUIRE(native.state.allocateCount == native.state.freeCount);
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

    struct EncodedCopy {
        VkBuffer source{};
        VkBuffer destination{};
        VkBufferCopy2 region{};
    };

    struct EncodedGraph {
        std::vector<EncodedCopy> copies;
        std::vector<VkBufferMemoryBarrier2> barriers;
        int renderingBegins{};
        int renderingEnds{};
    };

    EncodedGraph *encodedGraph{};

    VKAPI_ATTR void VKAPI_CALL CaptureBarrier(VkCommandBuffer, const VkDependencyInfo *info) {
        if (info->bufferMemoryBarrierCount != 0)
            encodedGraph->barriers.insert(encodedGraph->barriers.end(), info->pBufferMemoryBarriers,
                                          info->pBufferMemoryBarriers + info->bufferMemoryBarrierCount);
    }

    VKAPI_ATTR void VKAPI_CALL CaptureCopy(VkCommandBuffer, const VkCopyBufferInfo2 *info) {
        for (std::uint32_t index = 0; index < info->regionCount; ++index)
            encodedGraph->copies.push_back({info->srcBuffer, info->dstBuffer, info->pRegions[index]});
    }

    VKAPI_ATTR void VKAPI_CALL CaptureBeginRendering(VkCommandBuffer, const VkRenderingInfo *) {
        ++encodedGraph->renderingBegins;
    }

    VKAPI_ATTR void VKAPI_CALL CaptureEndRendering(VkCommandBuffer) {
        ++encodedGraph->renderingEnds;
    }

    [[nodiscard]] PFN_vkVoidFunction GraphResolver(const VkDevice device, const char *name) {
        if (const auto resource = FakeResourceResolver(device, name))
            return resource;
        if (const auto command = VulkanTest::Resolver(device, name))
            return command;
        const std::string_view symbol{name};
        if (symbol == "vkCmdPipelineBarrier2")
            return EraseFunction(PFN_vkCmdPipelineBarrier2{CaptureBarrier});
        if (symbol == "vkCmdCopyBuffer2")
            return EraseFunction(PFN_vkCmdCopyBuffer2{CaptureCopy});
        if (symbol == "vkCmdBeginRendering")
            return EraseFunction(PFN_vkCmdBeginRendering{CaptureBeginRendering});
        if (symbol == "vkCmdEndRendering")
            return EraseFunction(PFN_vkCmdEndRendering{CaptureEndRendering});
        return nullptr;
    }

    /** @brief Real resource and graph owners share native dispatch and survive assertion unwind until terminal release. */
    struct NativeGraphFixture {
        NativeResourceScope resourceState;
        VulkanTest::CommandFixture commandState;
        Detail::VulkanResourceRuntime resources;
        VulkanTest::Lease lease;
        EncodedGraph encoded;
        Detail::VulkanGraphRuntime graph;

        NativeGraphFixture() {
            encodedGraph = &encoded;
            InitializeNativeResourceRuntime(resources);
            REQUIRE(graph.Initialize(NativeHandle<VkDevice>(1), commandState.queues, 2, 1024, 11, resources, GraphResolver).HasValue());
        }

        ~NativeGraphFixture() {
            commandState.state.Complete();
            commandState.state.counterResult = VK_SUCCESS;
            (void)graph.BeginShutdown();
            (void)graph.PollDrain();
            encodedGraph = nullptr;
        }

        [[nodiscard]] std::array<std::uint64_t, 3> CreateBuffers(const bool initializeSource = true) {
            std::array<std::uint64_t, 3> result{};
            std::array<std::byte, 16> data{};
            for (std::size_t index = 0; index < result.size(); ++index) {
                const RenderBufferDescriptor descriptor{.byteSize = 16,
                                                        .usage = RenderBufferUsage::CopySource | RenderBufferUsage::CopyDestination,
                                                        .access = RenderBufferAccess::HostVisible};
                const auto plan = resources.QueryBufferMemoryCost(descriptor);
                REQUIRE(plan.HasValue());
                const auto created =
                    resources.CreateBuffer(descriptor,
                                           index == 0 && initializeSource ? std::span<const std::byte>{data} : std::span<const std::byte>{},
                                           TestSupport::PlacementFor(plan.Value(), index + 1));
                REQUIRE(created.HasValue());
                result[index] = created.Value();
            }
            return result;
        }

        void Bind(SubmissionTest::BufferGraph &packet, const std::array<std::uint64_t, 3> instances) {
            for (std::size_t index = 0; index < instances.size(); ++index)
                packet.instances[index].instance = instances[index];
        }
    };

    TEST_CASE("Vulkan graph encoding emits real copy commands and matched family ownership barriers",
              "[unit][runtime][renderer][vulkan][graph]") {
        NativeGraphFixture fixture;
        const auto instances = fixture.CreateBuffers();
        auto packet = SubmissionTest::CopyGraph();
        fixture.Bind(packet, instances);
        const auto records = packet.Submissions();
        RenderGraphLeaseTransfer transfer;
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        const auto executed =
            fixture.graph.ExecuteGraph({{1}, packet.graph, packet.workloads, packet.instances, &fixture.lease, false, &transfer, records});
        INFO(executed.HasError() ? executed.ErrorValue().message : "native graph admitted");
        REQUIRE(executed.HasValue());
        CHECK(transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
        REQUIRE(fixture.encoded.copies.size() == 2);
        CHECK(fixture.encoded.copies[0].destination == fixture.encoded.copies[1].source);
        CHECK(fixture.encoded.copies[0].source != fixture.encoded.copies[0].destination);
        CHECK(fixture.encoded.copies[1].region.size == 16);
        const auto release = std::ranges::find_if(fixture.encoded.barriers, [](const auto &barrier) {
            return barrier.srcQueueFamilyIndex == 3 && barrier.dstQueueFamilyIndex == 7 && barrier.dstStageMask == VK_PIPELINE_STAGE_2_NONE;
        });
        const auto acquire = std::ranges::find_if(fixture.encoded.barriers, [](const auto &barrier) {
            return barrier.srcQueueFamilyIndex == 3 && barrier.dstQueueFamilyIndex == 7 && barrier.srcStageMask == VK_PIPELINE_STAGE_2_NONE;
        });
        REQUIRE(release != fixture.encoded.barriers.end());
        REQUIRE(acquire != fixture.encoded.barriers.end());
        CHECK(release->buffer == acquire->buffer);
        CHECK(release->size == VK_WHOLE_SIZE);
        REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
        REQUIRE(fixture.commandState.state.accepted.size() == 2);
        REQUIRE(fixture.commandState.state.accepted[1].waits.size() == 1);
        CHECK(fixture.commandState.state.accepted[1].waits[0].semaphore == fixture.commandState.state.accepted[0].signal.semaphore);
        const int destroys = fixture.resourceState.state.destroyBufferCount;
        for (const auto instance : instances)
            fixture.resources.DestroyBuffer(instance);
        CHECK(fixture.resourceState.state.destroyBufferCount == destroys);
        REQUIRE(fixture.graph.BeginShutdown().HasValue());
        const auto pending = fixture.graph.PollDrain();
        REQUIRE(pending.HasValue());
        CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
        CHECK(fixture.lease.releases == 0);
        fixture.commandState.state.Complete();
        const auto drained = fixture.graph.PollDrain();
        REQUIRE(drained.HasValue());
        CHECK(drained.Value() == RenderBackendDrainProgress::Drained);
        CHECK(fixture.lease.releases == 1);
        CHECK(fixture.resourceState.state.destroyBufferCount == destroys + 3);
    }

    TEST_CASE("Vulkan graph rejects undefined copy contents and malformed native admission before recording",
              "[unit][runtime][renderer][vulkan][graph]") {
        NativeGraphFixture fixture;
        bool initialized = true;
        bool missingLease = false;
        SECTION("fresh source has no preserved contents") {
            initialized = false;
        }
        SECTION("resident work requires completion authority") {
            missingLease = true;
        }
        SECTION("native checked copy extent") {}
        const auto instances = fixture.CreateBuffers(initialized);
        auto packet = SubmissionTest::CopyGraph();
        fixture.Bind(packet, instances);
        if (initialized && !missingLease)
            std::get<RenderGraphBufferCopy>(packet.workloads[0].workload).byteCount = 17;
        const auto records = packet.Submissions();
        RenderGraphLeaseTransfer transfer;
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        const auto rejected = fixture.graph.ExecuteGraph(
            {{1}, packet.graph, packet.workloads, packet.instances, missingLease ? nullptr : &fixture.lease, false, &transfer, records});
        REQUIRE(rejected.HasError());
        CHECK(fixture.commandState.state.beginCalls == 0);
        CHECK(fixture.encoded.copies.empty());
        CHECK(fixture.commandState.state.accepted.empty());
        fixture.graph.AbortFrame({1});
    }

}  // namespace
