#pragma once

#include "../../../support/renderer/RenderGraphSubmissionFixture.h"
#include "VulkanCommandTestSupport.h"
#include "VulkanResourceTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanGraphRuntime.h"

#include <vector>

namespace Horo::Render::VulkanGraphTest {
    using namespace Horo::Render::VulkanResourceTest;

    struct EncodedCopy {
        VkBuffer source{};
        VkBuffer destination{};
        VkBufferCopy2 region{};
    };

    struct EncodedGraph {
        std::vector<EncodedCopy> copies;
        std::vector<VkBufferMemoryBarrier2> barriers;
        std::vector<VkImageMemoryBarrier2> imageBarriers;
        std::vector<std::string_view> operations;
        std::uint32_t layers{};
        bool depthAttachment{};
        int renderingBegins{};
        int renderingEnds{};
    };

    inline EncodedGraph *encodedGraph{};

    inline VKAPI_ATTR void VKAPI_CALL CaptureBarrier(VkCommandBuffer, const VkDependencyInfo *info) {
        encodedGraph->operations.push_back("barrier");
        if (info->imageMemoryBarrierCount != 0)
            encodedGraph->imageBarriers.insert(encodedGraph->imageBarriers.end(), info->pImageMemoryBarriers,
                                               info->pImageMemoryBarriers + info->imageMemoryBarrierCount);
        if (info->bufferMemoryBarrierCount != 0)
            encodedGraph->barriers.insert(encodedGraph->barriers.end(), info->pBufferMemoryBarriers,
                                          info->pBufferMemoryBarriers + info->bufferMemoryBarrierCount);
    }

    inline VKAPI_ATTR void VKAPI_CALL CaptureCopy(VkCommandBuffer, const VkCopyBufferInfo2 *info) {
        for (std::uint32_t index = 0; index < info->regionCount; ++index)
            encodedGraph->copies.push_back({info->srcBuffer, info->dstBuffer, info->pRegions[index]});
    }

    inline VKAPI_ATTR void VKAPI_CALL CaptureBeginRendering(VkCommandBuffer, const VkRenderingInfo *info) {
        encodedGraph->operations.push_back("begin-rendering");
        encodedGraph->layers = info->layerCount;
        encodedGraph->depthAttachment = info->pDepthAttachment != nullptr;
        ++encodedGraph->renderingBegins;
    }

    inline VKAPI_ATTR void VKAPI_CALL CaptureEndRendering(VkCommandBuffer) {
        encodedGraph->operations.push_back("end-rendering");
        ++encodedGraph->renderingEnds;
    }

    [[nodiscard]] inline PFN_vkVoidFunction GraphResolver(const VkDevice device, const char *name) {
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

}  // namespace Horo::Render::VulkanGraphTest
