#include "VulkanGraphTestSupport.h"
#include "VulkanPresentationTestSupport.h"

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
        std::array<std::byte, 64> pixels{};
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
    UploadPacket MakeUploadPacket(UploadResources resources, std::size_t bytes = 16,
                                  RenderTextureFormat format = RenderTextureFormat::Rgba8Unorm, std::uint64_t signalValue = 1) {
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
                {RenderGraphPassWorkload{pass, RenderGraphTextureUpload{buffer, texture, {2, 2}, bytes, format}}},
                {RenderGraphResourceInstance{buffer, resources.buffer}, RenderGraphResourceInstance{texture, resources.texture}},
                {RenderGraphQueueSubmission{{{1}, {1}, {}, {{1}, signalValue}}, {0, 1}}}};
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

TEST_CASE("Vulkan texture upload records the two exact analytic float layouts", "[renderer][vulkan][texture-upload][protocol-fake]") {
    NativeGraphFixture fixture;
    auto format = RenderTextureFormat::R32Float;
    std::size_t bytes = 16;
    SECTION("Single-channel float") {}
    SECTION("Four-channel float") {
        format = RenderTextureFormat::Rgba32Float;
        bytes = 64;
    }
    const auto resources = CreateUploadResources(fixture, true, bytes, format);
    auto packet = MakeUploadPacket(resources, bytes, format);
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    REQUIRE(fixture.graph.ExecuteGraph(Request(packet, fixture, transfer)).HasValue());
    CHECK(std::ranges::count(fixture.encoded.operations, "upload-image") == 1);
    fixture.graph.AbortFrame({1});
    REQUIRE(fixture.graph.BeginShutdown().HasValue());
    REQUIRE(fixture.graph.PollDrain().Value() == RenderBackendDrainProgress::Drained);
    fixture.resources.DestroyBuffer(resources.buffer);
    fixture.resources.DestroyTexture(resources.texture);
}

TEST_CASE("Vulkan WSI failure preserves accepted analytic table graph state", "[renderer][vulkan][texture-upload][wsi][protocol-fake]") {
    NativeGraphFixture fixture;
    VulkanPresentationTest::Fixture presentation;
    presentation.Realize();
    presentation.AcquireReady();
    constexpr FrameToken frame{1};
    constexpr std::uint64_t deviceIncarnation = 11;
    REQUIRE(presentation.runtime.BeginFrame(frame).HasValue());

    const auto resources = CreateUploadResources(fixture, true, 64, RenderTextureFormat::Rgba32Float);
    auto packet = MakeUploadPacket(resources, 64, RenderTextureFormat::Rgba32Float, 3);
    const auto signal = packet.submissions.front().submission.signal;
    RenderGraphLeaseTransfer transfer;
    const auto request = Request(packet, fixture, transfer, frame);
    REQUIRE(fixture.graph.BeginFrame(frame).HasValue());
    REQUIRE(fixture.graph.ExecuteGraph(request).HasValue());
    CHECK_FALSE(fixture.graph.WasSubmissionAccepted(signal, deviceIncarnation).Value());

    Detail::VulkanCommandRetention capture;
    capture.resources.reserve(packet.instances.size());
    std::array<Detail::VulkanGraphResource, 2> bindings;
    REQUIRE(fixture.resources.CaptureGraphResources(request, bindings, capture).HasValue());
    const auto &table = *bindings[packet.instances.size() - 1].state;
    CHECK_FALSE(table.contentsDefined);

    REQUIRE(fixture.graph.SubmitFrame(frame).HasValue());
    REQUIRE(presentation.runtime.Submitted(frame, signal).HasValue());
    presentation.state.presentResult = VK_ERROR_OUT_OF_DATE_KHR;
    REQUIRE(presentation.runtime.Present(frame).HasError());
    fixture.graph.AbortFrame(frame);
    presentation.runtime.Abort(frame);

    const auto counterCalls = fixture.commandState.state.counterCalls;
    REQUIRE(fixture.graph.WasSubmissionAccepted(signal, deviceIncarnation).Value());
    CHECK_FALSE(fixture.graph.WasSubmissionAccepted({signal.queue, 2}, deviceIncarnation).Value());
    CHECK(fixture.graph.WasSubmissionAccepted(signal, deviceIncarnation + 1).HasError());
    CHECK(fixture.commandState.state.counterCalls == counterCalls);
    CHECK(fixture.commandState.state.waitCalls == 0);
    CHECK(fixture.lease.releases == 0);
    CHECK(table.contentsDefined);
    CHECK(table.completion == signal);
    CHECK(table.logical.layout == RenderGraphTextureLayout::CopyDestination);

    fixture.commandState.state.Complete();
    REQUIRE(fixture.graph.BeginShutdown().HasValue());
    REQUIRE(fixture.graph.PollDrain().Value() == RenderBackendDrainProgress::Drained);
    CHECK(fixture.lease.releases == 1);
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
    SECTION("native format differs from authored RGBA8 despite equal byte length") {
        format = RenderTextureFormat::R32Float;
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
    CHECK_FALSE(fixture.graph.WasSubmissionAccepted(packet.submissions.front().submission.signal, 11).Value());
    CHECK(fixture.lease.releases == 1);
    RenderGraphLeaseTransfer retry;
    REQUIRE(fixture.graph.BeginFrame({2}).HasValue());
    REQUIRE(fixture.graph.ExecuteGraph(Request(packet, fixture, retry, {2})).HasValue());
    REQUIRE(fixture.graph.SubmitFrame({2}).HasValue());
    CHECK(fixture.commandState.state.accepted.size() == 1);
    CHECK(std::ranges::count(fixture.encoded.operations, "upload-image") == 2);
    CHECK(fixture.commandState.state.waitCalls == 0);
}
