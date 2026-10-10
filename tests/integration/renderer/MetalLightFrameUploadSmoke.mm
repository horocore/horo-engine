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
        /** @brief Allocates the four bounded host-visible storage tables for a one-light frame. */
        std::array<std::uint64_t, 4> CreateFrameBuffers(MetalResourceRuntime &runtime) {
            const std::array<std::size_t, 4> sizes{sizeof(PackedRenderLight), sizeof(PackedLightCluster), sizeof(PackedLightMembership),
                                                   sizeof(std::uint32_t)};
            std::array<std::uint64_t, 4> instances{};
            for (std::size_t index = 0; index < sizes.size(); ++index) {
                const RenderBufferDescriptor descriptor{.byteSize = sizes[index],
                                                        .usage = RenderBufferUsage::Storage | RenderBufferUsage::CopySource,
                                                        .access = RenderBufferAccess::HostVisible};
                const auto cost = runtime.QueryBufferMemoryCost(descriptor);
                REQUIRE(cost.HasValue());
                const auto created = runtime.CreateBuffer(descriptor, {}, TestSupport::PlacementFor(cost.Value(), index + 1, index + 1));
                REQUIRE(created.HasValue());
                instances[index] = created.Value();
            }
            return instances;
        }

        /** @brief Waits only within the smoke-test deadline for actual command completion. */
        void RequireCompletion(id<MTLCommandBuffer> commands) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            REQUIRE(commands.status == MTLCommandBufferStatusCompleted);
        }

        /** @brief Confirms rejected stale, aliased and cancelled updates leave the published revision unchanged. */
        void CheckRejectedUpdates(MetalResourceRuntime &runtime, NativeLightFrameUpdate update, const MetalBufferInstance &resident) {
            CHECK(runtime.UpdateLightFrame(update).HasError());
            auto invalid = update;
            invalid.update.revision = 3;
            invalid.instances[3] = invalid.instances[0];
            CHECK(runtime.UpdateLightFrame(invalid).HasError());
            CHECK(resident.lightTableRevision == 2);
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            update.update.revision = 3;
            update.update.cancellation = cancellation.Token();
            CHECK(runtime.UpdateLightFrame(update).HasError());
            CHECK(resident.lightTableRevision == 2);
        }
    }  // namespace

    TEST_CASE("Metal light upload requires actual completion before changing any reusable slot",
              "[integration][renderer][metal][gpu][light]") {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        REQUIRE(device != nil);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        REQUIRE(queue != nil);
        MetalResourceRuntime runtime;
        runtime.Initialize((__bridge void *)device, (__bridge void *)queue);
        const LightCullingBudget budget{.maximumLights = 1, .maximumClusters = 1, .referencesPerCluster = 1};
        const auto instances = CreateFrameBuffers(runtime);
        std::array<PackedRenderLight, 1> lights{};
        lights[0].direction = {0, 0, -1};
        lights[0].colorIntensity = {1, 1, 1, 1};
        lights[0].cones = {1, 0};
        lights[0].identityLow = 1;
        std::array<PackedLightCluster, 1> clusters{};
        for (auto &plane : clusters[0].planes)
            plane = {1, 0, 0, 1};
        NativeLightFrameUpdate update{instances, {.budget = budget, .revision = 1, .lights = lights, .clusters = clusters}};
        REQUIRE(runtime.UpdateLightFrame(update).HasValue());
        auto *resident = reinterpret_cast<MetalBufferInstance *>(static_cast<std::uintptr_t>(instances[0]));
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        REQUIRE(commands != nil);
        resident->use.last = commands;
        update.update.revision = 2;
        lights[0].colorIntensity[3] = 2;
        const auto pending = runtime.UpdateLightFrame(update);
        REQUIRE(pending.HasError());
        CHECK(pending.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value());
        CHECK(resident->lightTableRevision == 1);
        CHECK(static_cast<const PackedRenderLight *>(resident->buffer.contents)->colorIntensity[3] == 1);
        [commands commit];
        RequireCompletion(commands);
        REQUIRE(runtime.UpdateLightFrame(update).HasValue());
        CHECK(resident->lightTableRevision == 2);
        CHECK(static_cast<const PackedRenderLight *>(resident->buffer.contents)->colorIntensity[3] == 2);
        CheckRejectedUpdates(runtime, update, *resident);
        for (const auto instance : instances)
            runtime.DestroyBuffer(instance);
        runtime.Shutdown();
    }
}  // namespace Horo::Render::Detail
