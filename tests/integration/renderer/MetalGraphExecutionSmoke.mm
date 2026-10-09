#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"
#include "runtime/renderer/modules/metal/MetalResourceRuntime.h"

#import <Metal/Metal.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <thread>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Creates and retains the native device/queue in the test runtime. */
        id<MTLCommandQueue> InitializeSmokeRuntime(MetalResourceRuntime &runtime) {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            REQUIRE(device != nil);
            id<MTLCommandQueue> queue = [device newCommandQueue];
            REQUIRE(queue != nil);
            runtime.Initialize((__bridge void *)device, (__bridge void *)queue);
            return queue;
        }

        /** @brief Requires exact native terminal completion within a finite smoke-test budget. */
        void RequireNativeCompletion(id<MTLCommandBuffer> commands) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            REQUIRE(commands.status == MTLCommandBufferStatusCompleted);
        }
    }  // namespace

    TEST_CASE("Metal graph buffer copy validates ranges and records actual resident bytes", "[integration][renderer][metal][gpu]") {
        MetalResourceRuntime runtime;
        id<MTLCommandQueue> queue = InitializeSmokeRuntime(runtime);
        const std::array<std::byte, 16> sourceBytes{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
        const RenderBufferDescriptor sourceDescriptor{.byteSize = sourceBytes.size(),
                                                      .usage = RenderBufferUsage::CopySource,
                                                      .access = RenderBufferAccess::HostVisible};
        const RenderBufferDescriptor destinationDescriptor{.byteSize = sourceBytes.size(),
                                                           .usage = RenderBufferUsage::CopyDestination,
                                                           .access = RenderBufferAccess::HostVisible};
        const auto sourceCost = runtime.QueryBufferMemoryCost(sourceDescriptor);
        const auto destinationCost = runtime.QueryBufferMemoryCost(destinationDescriptor);
        REQUIRE(sourceCost.HasValue());
        REQUIRE(destinationCost.HasValue());
        const auto source = runtime.CreateBuffer(sourceDescriptor, sourceBytes, TestSupport::PlacementFor(sourceCost.Value(), 1, 1));
        const auto destination = runtime.CreateBuffer(destinationDescriptor, {}, TestSupport::PlacementFor(destinationCost.Value(), 2, 2));
        REQUIRE(source.HasValue());
        REQUIRE(destination.HasValue());
        const RenderGraphResourceId sourceId{{1}, 1};
        const RenderGraphResourceId destinationId{{1}, 2};
        const std::array resources{RenderGraphResourceInstance{sourceId, source.Value()},
                                   RenderGraphResourceInstance{destinationId, destination.Value()}};
        RenderGraphWorkload workload = RenderGraphBufferCopy{sourceId, destinationId, 0, 4, 4};
        REQUIRE(runtime.ValidateGraphWorkload(workload, resources).HasValue());
        auto invalid = workload;
        std::get<RenderGraphBufferCopy>(invalid).byteCount = sourceBytes.size() + 1;
        REQUIRE(runtime.ValidateGraphWorkload(invalid, resources).HasError());
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        REQUIRE(runtime.ExecuteGraphWorkload((__bridge void *)commands, workload, resources).HasValue());
        SECTION("unsent-frame abandonment releases placement") {
            runtime.FinishGraphCommands((__bridge void *)commands, false);
            commands = nil;
            runtime.DestroyBuffer(source.Value());
            REQUIRE(runtime.CreateBuffer(sourceDescriptor, sourceBytes, TestSupport::PlacementFor(sourceCost.Value(), 3, 1)).HasValue());
            runtime.Shutdown();
            return;
        }
        SECTION("submitted copy pins placement until completion") {}
        runtime.DestroyBuffer(source.Value());
        REQUIRE(runtime.ValidateGraphWorkload(workload, resources).HasError());
        REQUIRE(runtime.CreateBuffer(sourceDescriptor, sourceBytes, TestSupport::PlacementFor(sourceCost.Value(), 3, 1)).HasError());
        [commands commit];
        RequireNativeCompletion(commands);
        auto *resident = reinterpret_cast<MetalBufferInstance *>(static_cast<std::uintptr_t>(destination.Value()));
        REQUIRE(std::memcmp(static_cast<const std::byte *>(resident->buffer.contents) + 4, sourceBytes.data(), 4) == 0);
        runtime.DrainGraphRetirements();
        REQUIRE(runtime.CreateBuffer(sourceDescriptor, sourceBytes, TestSupport::PlacementFor(sourceCost.Value(), 4, 1)).HasValue());
        runtime.Shutdown();
    }

    TEST_CASE("Metal graph color clear and load preserve actual tracked heap contents", "[integration][renderer][metal][gpu]") {
        MetalResourceRuntime runtime;
        id<MTLCommandQueue> queue = InitializeSmokeRuntime(runtime);
        const RenderTextureDescriptor descriptor{.extent = {1, 1},
                                                 .format = RenderTextureFormat::Rgba8Unorm,
                                                 .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource};
        const auto textureCost = runtime.QueryTextureMemoryCost(descriptor);
        REQUIRE(textureCost.HasValue());
        const auto texture = runtime.CreateTexture(descriptor, {}, TestSupport::PlacementFor(textureCost.Value(), 1, 1));
        REQUIRE(texture.HasValue());
        const RenderBufferDescriptor readbackDescriptor{.byteSize = 256,
                                                        .usage = RenderBufferUsage::CopyDestination,
                                                        .access = RenderBufferAccess::HostVisible};
        const auto readbackCost = runtime.QueryBufferMemoryCost(readbackDescriptor);
        REQUIRE(readbackCost.HasValue());
        const auto readback = runtime.CreateBuffer(readbackDescriptor, {}, TestSupport::PlacementFor(readbackCost.Value(), 2, 2));
        REQUIRE(readback.HasValue());
        const RenderGraphResourceId textureId{{1}, 1};
        const std::array resources{RenderGraphResourceInstance{textureId, texture.Value()}};
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        const RenderGraphWorkload clear = RenderGraphColorAttachment{textureId, {.clearColor = {1, 0, 0, 1}}};
        const RenderGraphWorkload load = RenderGraphColorAttachment{textureId, {.loadOperation = AttachmentLoadOperation::Load}};
        REQUIRE(runtime.ExecuteGraphWorkload((__bridge void *)commands, clear, resources).HasValue());
        REQUIRE(runtime.ExecuteGraphWorkload((__bridge void *)commands, load, resources).HasValue());
        auto *resident = reinterpret_cast<MetalTextureInstance *>(static_cast<std::uintptr_t>(texture.Value()));
        auto *destination = reinterpret_cast<MetalBufferInstance *>(static_cast<std::uintptr_t>(readback.Value()));
        CHECK(resident->texture.hazardTrackingMode == MTLHazardTrackingModeTracked);
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        REQUIRE(blit != nil);
        [blit copyFromTexture:resident->texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(1, 1, 1)
                            toBuffer:destination->buffer
                   destinationOffset:0
              destinationBytesPerRow:256
            destinationBytesPerImage:256];
        [blit endEncoding];
        [commands commit];
        RequireNativeCompletion(commands);
        const auto *pixel = static_cast<const unsigned char *>(destination->buffer.contents);
        CHECK(pixel[0] == 255);
        CHECK(pixel[1] == 0);
        CHECK(pixel[2] == 0);
        CHECK(pixel[3] == 255);
        runtime.Shutdown();
    }
}  // namespace Horo::Render::Detail
