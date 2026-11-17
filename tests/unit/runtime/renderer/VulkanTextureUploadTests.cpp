#include "VulkanGraphTestSupport.h"

#include <algorithm>
#include <array>
#include <utility>

namespace {
    using namespace Horo::Render;
    using namespace Horo::Render::VulkanGraphTest;

    /** @brief Real resource/command owner protocol fixture with fake Vulkan functions; this is never GPU execution evidence. */
    struct UploadResources final {
        std::uint64_t buffer{}, texture{};
    };

    /** @brief Creates actual registered owner instances and host-uploaded bytes through the existing resource path. */
    UploadResources CreateUploadResources(NativeGraphFixture &fixture, bool initialize = true, std::size_t bytes = 16,
                                          RenderTextureFormat format = RenderTextureFormat::Rgba8Unorm) {
        std::array<std::byte, 20> pixels{};
        const RenderBufferDescriptor buffer{bytes, RenderBufferUsage::CopySource, RenderBufferAccess::HostVisible};
        const auto bufferPlan = fixture.resources.QueryBufferMemoryCost(buffer);
        REQUIRE(bufferPlan.HasValue());
        const auto bufferInstance =
            fixture.resources.CreateBuffer(buffer,
                                           initialize ? std::span<const std::byte>{pixels}.first(bytes) : std::span<const std::byte>{},
                                           TestSupport::PlacementFor(bufferPlan.Value(), 1));
        REQUIRE(bufferInstance.HasValue());
        const RenderTextureDescriptor texture{.extent = {2, 2},
                                              .format = format,
                                              .usage = RenderTextureUsage::CopyDestination | RenderTextureUsage::Sampled};
        const auto texturePlan = fixture.resources.QueryTextureMemoryCost(texture);
        REQUIRE(texturePlan.HasValue());
        const auto textureInstance = fixture.resources.CreateTexture(texture, {}, TestSupport::PlacementFor(texturePlan.Value(), 2));
        REQUIRE(textureInstance.HasValue());
        return {bufferInstance.Value(), textureInstance.Value()};
    }

    /** @brief Owns a real portable upload graph and exact submission packet against the fixture's registered native generations. */
    struct UploadPacket final {
        CompiledRenderGraphExecution graph;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::array<RenderGraphResourceInstance, 2> instances;
        std::array<RenderGraphQueueSubmission, 1> submissions;
    };

    /** @brief Authors the real closed copy workload with exact imported-state and submission evidence. */
    UploadPacket MakeUploadPacket(UploadResources resources) {
        using SubmissionTest::Take;
        auto builder = Take(RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 2, .maxUsages = 2, .maxDependencies = 1}));
        const auto pass = Take(builder.AddPass(RenderPassKind::Copy, RenderQueueRole::Graphics));
        const auto buffer = Take(builder.ImportBuffer(RenderBufferHandle{{41}, 1, 1}, RenderGraphResourceClass::Persistent));
        const auto texture = Take(builder.ImportTexture(RenderTextureHandle{{41}, 2, 1}, RenderGraphResourceClass::Persistent));
        REQUIRE(builder.AddUsage({pass, buffer, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource}).HasValue());
        REQUIRE(builder.AddUsage({pass, texture, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination}).HasValue());
        const auto authored = Take(builder.Finalize());
        const auto schedule = Take(CompileRenderGraph(authored));
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const std::array initial{RenderGraphImportedState{buffer,
                                                          {RenderGraphSynchronizationAccess::Read,
                                                           RenderGraphSynchronizationOperation::CopySource,
                                                           RenderGraphPipelineScope::Transfer,
                                                           RenderGraphTextureLayout::NotApplicable,
                                                           {1}}},
                                 RenderGraphImportedState{texture, {}, RenderGraphInitialContents::Discarded}};
        const auto synchronization = Take(SynthesizeRenderGraphSynchronization(authored, schedule, queues, initial));
        auto graph = Take(CompileRenderGraphExecution(authored, schedule, synchronization, queues));
        return {std::move(graph),
                {RenderGraphPassWorkload{pass, RenderGraphTextureUpload{buffer, texture, {2, 2}, 16}}},
                {RenderGraphResourceInstance{buffer, resources.buffer}, RenderGraphResourceInstance{texture, resources.texture}},
                {RenderGraphQueueSubmission{{{1}, {1}, {}, {{1}, 1}}, {0, 1}}}};
    }

    /** @brief Executes only through the existing actual graph owner and submission/lease protocol. */
    auto Request(UploadPacket &packet, NativeGraphFixture &fixture, RenderGraphLeaseTransfer &transfer, FrameToken frame = {1}) {
        return RenderGraphExecutionRequest{frame,          packet.graph, packet.workloads, packet.instances,
                                           &fixture.lease, false,        &transfer,        packet.submissions};
    }
}  // namespace

TEST_CASE("Vulkan texture upload protocol retains registered resources until actual command-owner completion",
          "[renderer][vulkan][texture-upload][protocol-fake]") {
    NativeGraphFixture fixture;
    const auto resources = CreateUploadResources(fixture);
    auto packet = MakeUploadPacket(resources);
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    REQUIRE(fixture.graph.ExecuteGraph(Request(packet, fixture, transfer)).HasValue());
    CHECK(std::ranges::count(fixture.encoded.operations, "upload-image") == 1);
    CHECK(fixture.commandState.state.accepted.empty());
    CHECK(transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
    REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
    REQUIRE(fixture.commandState.state.accepted.size() == 1);
    const auto buffersBefore = fixture.resourceState.state.destroyBufferCount;
    const auto imagesBefore = fixture.resourceState.state.destroyImageCount;
    fixture.resources.DestroyBuffer(resources.buffer);
    fixture.resources.DestroyTexture(resources.texture);
    CHECK(fixture.resourceState.state.destroyBufferCount == buffersBefore);
    CHECK(fixture.resourceState.state.destroyImageCount == imagesBefore);
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
    CHECK(fixture.resourceState.state.destroyBufferCount == buffersBefore + 1);
    CHECK(fixture.resourceState.state.destroyImageCount == imagesBefore + 1);
    CHECK(fixture.commandState.state.waitCalls == 0);
}

TEST_CASE("Vulkan texture upload protocol rejects undefined or mismatched native resources before recording",
          "[renderer][vulkan][texture-upload][protocol-fake]") {
    NativeGraphFixture fixture;
    bool initialize = true;
    std::size_t bytes = 16;
    auto format = RenderTextureFormat::Rgba8Unorm;
    SECTION("undefined host source") {
        initialize = false;
    }
    SECTION("padded source differs from exact payload") {
        bytes = 20;
    }
    SECTION("native format differs from closed RGBA8") {
        format = RenderTextureFormat::Rgba16Float;
    }
    const auto resources = CreateUploadResources(fixture, initialize, bytes, format);
    auto packet = MakeUploadPacket(resources);
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    REQUIRE(fixture.graph.ExecuteGraph(Request(packet, fixture, transfer)).HasError());
    CHECK(fixture.commandState.state.beginCalls == 0);
    CHECK(fixture.commandState.state.accepted.empty());
    CHECK(std::ranges::count(fixture.encoded.operations, "upload-image") == 0);
    CHECK(fixture.commandState.state.waitCalls == 0);
    fixture.graph.AbortFrame({1});
}

TEST_CASE("Vulkan unsent texture upload abort leaves the same discarded destination retryable",
          "[renderer][vulkan][texture-upload][protocol-fake]") {
    NativeGraphFixture fixture;
    const auto resources = CreateUploadResources(fixture);
    auto packet = MakeUploadPacket(resources);
    RenderGraphLeaseTransfer first;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    REQUIRE(fixture.graph.ExecuteGraph(Request(packet, fixture, first)).HasValue());
    fixture.graph.AbortFrame({1});
    CHECK(fixture.commandState.state.accepted.empty());
    CHECK(fixture.lease.releases == 1);
    RenderGraphLeaseTransfer retry;
    REQUIRE(fixture.graph.BeginFrame({2}).HasValue());
    REQUIRE(fixture.graph.ExecuteGraph(Request(packet, fixture, retry, {2})).HasValue());
    REQUIRE(fixture.graph.SubmitFrame({2}).HasValue());
    CHECK(fixture.commandState.state.accepted.size() == 1);
    CHECK(std::ranges::count(fixture.encoded.operations, "upload-image") == 2);
    CHECK(fixture.commandState.state.waitCalls == 0);
}
