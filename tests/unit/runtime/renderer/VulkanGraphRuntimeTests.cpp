#include "../../../support/renderer/RenderGraphSubmissionFixture.h"
#include "VulkanCommandTestSupport.h"
#include "VulkanResourceTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanGraphRuntime.h"
#include "support/renderer/MultiviewTestSupport.h"

#if defined(HORO_TEST_OPENXR_VULKAN)
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenXRMultiviewGraph.h"
#include "OpenXRSwapchainTestSupport.h"
#include "OpenXRVulkanSwapchain.h"
#include "support/TypedIdentityTestSupport.h"
#endif

namespace {
    using namespace Horo::Render;
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
        std::vector<std::string> operations;
        std::uint32_t layers{};
        bool depthAttachment{};

        struct DrawCall {
            std::uint32_t indices{}, instances{}, firstView{};
        };

        std::vector<DrawCall> drawCalls;
        int renderingBegins{};
        int renderingEnds{};
        int pipelineBinds{}, descriptorBinds{}, vertexBinds{}, indexBinds{}, draws{};
    };

    EncodedGraph *encodedGraph{};

    VKAPI_ATTR void VKAPI_CALL CaptureBarrier(VkCommandBuffer, const VkDependencyInfo *info) {
        encodedGraph->operations.emplace_back("barrier");
        if (info->bufferMemoryBarrierCount != 0)
            encodedGraph->barriers.insert(encodedGraph->barriers.end(), info->pBufferMemoryBarriers,
                                          info->pBufferMemoryBarriers + info->bufferMemoryBarrierCount);
        if (info->imageMemoryBarrierCount != 0)
            encodedGraph->imageBarriers.insert(encodedGraph->imageBarriers.end(), info->pImageMemoryBarriers,
                                               info->pImageMemoryBarriers + info->imageMemoryBarrierCount);
    }

    VKAPI_ATTR void VKAPI_CALL CaptureCopy(VkCommandBuffer, const VkCopyBufferInfo2 *info) {
        for (std::uint32_t index = 0; index < info->regionCount; ++index)
            encodedGraph->copies.push_back({info->srcBuffer, info->dstBuffer, info->pRegions[index]});
    }

    VKAPI_ATTR void VKAPI_CALL CaptureBeginRendering(VkCommandBuffer, const VkRenderingInfo *info) {
        encodedGraph->operations.emplace_back("begin-rendering");
        encodedGraph->layers = info->layerCount;
        encodedGraph->depthAttachment = info->pDepthAttachment != nullptr;
        ++encodedGraph->renderingBegins;
    }

    VKAPI_ATTR void VKAPI_CALL CaptureEndRendering(VkCommandBuffer) {
        encodedGraph->operations.emplace_back("end-rendering");
        ++encodedGraph->renderingEnds;
    }

    VKAPI_ATTR void VKAPI_CALL CapturePipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {
        ++encodedGraph->pipelineBinds;
    }

    VKAPI_ATTR void VKAPI_CALL CaptureSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, std::uint32_t, std::uint32_t,
                                           const VkDescriptorSet *, std::uint32_t, const std::uint32_t *) {
        ++encodedGraph->descriptorBinds;
    }

    VKAPI_ATTR void VKAPI_CALL CaptureVertices(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkBuffer *, const VkDeviceSize *) {
        ++encodedGraph->vertexBinds;
    }

    VKAPI_ATTR void VKAPI_CALL CaptureIndex(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType) {
        ++encodedGraph->indexBinds;
    }

    VKAPI_ATTR void VKAPI_CALL CaptureViewport(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkViewport *) {}

    VKAPI_ATTR void VKAPI_CALL CaptureScissor(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkRect2D *) {}

    VKAPI_ATTR void VKAPI_CALL CaptureDraw(VkCommandBuffer, std::uint32_t count, std::uint32_t instances, std::uint32_t, std::int32_t,
                                           std::uint32_t firstView) {
        CHECK(count == 3);
        CHECK(instances > 0);
        CHECK(instances <= MultiviewExtractionLimits::HardMaximumViews);
        encodedGraph->drawCalls.push_back({count, instances, firstView});
        ++encodedGraph->draws;
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

        struct Entry {
            std::string_view name;
            PFN_vkVoidFunction function;
        };

        const std::array entries{Entry{"vkCmdBindPipeline", EraseFunction(PFN_vkCmdBindPipeline{CapturePipeline})},
                                 Entry{"vkCmdBindDescriptorSets", EraseFunction(PFN_vkCmdBindDescriptorSets{CaptureSets})},
                                 Entry{"vkCmdBindVertexBuffers", EraseFunction(PFN_vkCmdBindVertexBuffers{CaptureVertices})},
                                 Entry{"vkCmdBindIndexBuffer", EraseFunction(PFN_vkCmdBindIndexBuffer{CaptureIndex})},
                                 Entry{"vkCmdSetViewport", EraseFunction(PFN_vkCmdSetViewport{CaptureViewport})},
                                 Entry{"vkCmdSetScissor", EraseFunction(PFN_vkCmdSetScissor{CaptureScissor})},
                                 Entry{"vkCmdDrawIndexed", EraseFunction(PFN_vkCmdDrawIndexed{CaptureDraw})}};
        const auto found = std::ranges::find(entries, symbol, &Entry::name);
        return found == entries.end() ? nullptr : found->function;
    }

    /** @brief Real resource and graph owners share native dispatch and survive assertion unwind until terminal release. */
    struct NativeGraphFixture {
        NativeResourceScope resourceState;
        VulkanTest::CommandFixture commandState;
        Detail::VulkanResourceRuntime resources;
        VulkanTest::Lease lease;
        EncodedGraph encoded;
        Detail::VulkanGraphRuntime graph;

        explicit NativeGraphFixture(const bool vertexLayers = false) {
            encodedGraph = &encoded;
            InitializeNativeResourceRuntime(resources);
            REQUIRE(graph.Initialize(NativeHandle<VkDevice>(1), commandState.queues, 2, 1024, 11, resources, GraphResolver, vertexLayers)
                        .HasValue());
        }

        ~explicit NativeGraphFixture(const bool vertexLayers = false) {
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

#include "VulkanMaterialCommandTests.inl"
#include "VulkanMultiviewCommandTests.inl"

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

#include "VulkanExternalImageTests.inl"
}  // namespace
