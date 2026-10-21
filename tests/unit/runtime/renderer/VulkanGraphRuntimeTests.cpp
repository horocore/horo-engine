#include "VulkanGraphTestSupport.h"
#include "support/renderer/MultiviewTestSupport.h"

namespace {
    using namespace Horo::Render;
    using namespace Horo::Render::VulkanResourceTest;
    using namespace Horo::Render::VulkanGraphTest;
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

}  // namespace
