#include "../../../support/renderer/MultiviewTestSupport.h"
#include "VulkanGraphTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanGraphRuntimeInternal.h"

namespace {
    using namespace Horo::Render;
    using namespace Horo::Render::VulkanResourceTest;
    using namespace Horo::Render::VulkanGraphTest;

#include "VulkanMaterialCommandTests.inl"
#include "VulkanMultiviewCommandTests.inl"

    TEST_CASE("Vulkan VFX publication rejects active recording and closed admission without native writes or waits",
              "[unit][runtime][renderer][vulkan][vfx]") {
        NativeGraphFixture fixture;
        VfxFrameWriteStatus status{VfxFrameWriteStatus::NotStarted};
        NativeVfxFrameUpdate update{.instance = 999, .revision = 1, .instanceCount = 1, .writeStatus = &status, .deviceIncarnation = 11};
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        REQUIRE(fixture.graph.UpdateVfxFrame(update).HasError());
        CHECK(status == VfxFrameWriteStatus::NotStarted);
        CHECK(fixture.commandState.state.waitCalls == 0);
        CHECK(fixture.encoded.draws == 0);
        fixture.graph.AbortFrame({1});
        REQUIRE(fixture.graph.BeginShutdown().HasValue());
        REQUIRE(fixture.graph.UpdateVfxFrame(update).HasError());
        CHECK(status == VfxFrameWriteStatus::NotStarted);
        CHECK(fixture.commandState.state.waitCalls == 0);
    }

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
        INFO((executed.HasError() ? executed.ErrorValue().message : "native graph admitted"));
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

    TEST_CASE("Vulkan canonical frame slot survives skipped recordings and never reuses pending native work",
              "[renderer][vulkan][graph][retirement]") {
        NativeGraphFixture fixture;
        const auto instances = fixture.CreateBuffers();
        auto packet = SubmissionTest::CopyGraph();
        fixture.Bind(packet, instances);
        const auto records = packet.Submissions();
        RenderGraphLeaseTransfer transfer;
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        const auto first = fixture.graph.FrameSlot({1});
        REQUIRE(first.HasValue());
        CHECK(first.Value().count == 2);
        CHECK(fixture.graph.CanWriteFrameInputs({1}));
        CHECK(fixture.graph.FrameSlot({2}).HasError());
        REQUIRE(
            fixture.graph.ExecuteGraph({{1}, packet.graph, packet.workloads, packet.instances, &fixture.lease, false, &transfer, records})
                .HasValue());
        CHECK_FALSE(fixture.graph.CanWriteFrameInputs({1}));
        REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
        REQUIRE(fixture.graph.BeginFrame({2}).HasValue());
        const auto second = fixture.graph.FrameSlot({2});
        REQUIRE(second.HasValue());
        CHECK(second.Value().index != first.Value().index);
        fixture.graph.AbortFrame({2});
        REQUIRE(fixture.graph.BeginFrame({3}).HasValue());
        const auto skipped = fixture.graph.FrameSlot({3});
        REQUIRE(skipped.HasValue());
        CHECK(skipped.Value().index == second.Value().index);
        CHECK(fixture.graph.CanWriteFrameInputs({3}));
        fixture.graph.AbortFrame({3});
        CHECK(fixture.lease.releases == 0);
        fixture.commandState.state.Complete();
        REQUIRE(fixture.graph.BeginFrame({4}).HasValue());
        const auto retired = fixture.graph.FrameSlot({4});
        REQUIRE(retired.HasValue());
        CHECK(retired.Value().index == first.Value().index);
        CHECK(fixture.lease.releases == 1);
        CHECK(fixture.commandState.state.waitCalls == 0);
        fixture.graph.AbortFrame({4});
    }

}  // namespace

TEST_CASE("Vulkan compute barrier scope requires actual queue evidence", "[unit][runtime][renderer][vulkan][vfx]") {
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;
    const VulkanGraphResource buffer{.kind = RenderGraphResourceKind::Buffer};
    const RenderGraphLogicalState before{};
    const RenderGraphLogicalState after{RenderGraphSynchronizationAccess::ReadWrite,
                                        RenderGraphSynchronizationOperation::Storage,
                                        RenderGraphPipelineScope::Compute,
                                        RenderGraphTextureLayout::NotApplicable,
                                        {1}};
    CHECK(TranslateVulkanGraphBarrier(buffer, before, after).HasError());
    const auto admitted = TranslateVulkanGraphBarrier(buffer, before, after, {.computeAvailable = true});
    REQUIRE(admitted.HasValue());
    CHECK(admitted.Value().buffer.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(admitted.Value().buffer.dstAccessMask == (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT));
}

TEST_CASE("Vulkan private particle handoff never opens generic external image or write access", "[unit][runtime][renderer][vulkan][vfx]") {
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;
    VulkanGraphResource resource{.kind = RenderGraphResourceKind::Buffer};
    const RenderGraphLogicalState before{RenderGraphSynchronizationAccess::Write,
                                         RenderGraphSynchronizationOperation::Storage,
                                         RenderGraphPipelineScope::Compute,
                                         RenderGraphTextureLayout::NotApplicable,
                                         {1}};
    RenderGraphLogicalState after{RenderGraphSynchronizationAccess::Read,
                                  RenderGraphSynchronizationOperation::Storage,
                                  RenderGraphPipelineScope::External,
                                  RenderGraphTextureLayout::NotApplicable,
                                  {1}};
    CHECK(TranslateVulkanGraphBarrier(resource, before, after, {.computeAvailable = true}).HasError());
    const VulkanGraphBarrierOptions exact{.computeAvailable = true, .particleHandoff = true};
    const auto admitted = TranslateVulkanGraphBarrier(resource, before, after, exact);
    REQUIRE(admitted.HasValue());
    CHECK(admitted.Value().buffer.srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    CHECK(admitted.Value().buffer.dstStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    CHECK(admitted.Value().buffer.dstAccessMask == VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    after.access = RenderGraphSynchronizationAccess::Write;
    CHECK(TranslateVulkanGraphBarrier(resource, before, after, exact).HasError());
    after.access = RenderGraphSynchronizationAccess::Read;
    resource.kind = RenderGraphResourceKind::Texture;
    CHECK(TranslateVulkanGraphBarrier(resource, before, after, exact).HasError());
}
