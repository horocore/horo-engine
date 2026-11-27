#include "../../../support/renderer/MultiviewTestSupport.h"
#include "VulkanGraphTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanGraphRuntimeInternal.h"

#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo::Render;
    using namespace Horo::Render::VulkanResourceTest;
    using namespace Horo::Render::VulkanGraphTest;

#include "VulkanMaterialCommandTests.inl"
#include "VulkanMultiviewCommandTests.inl"

    TEST_CASE("Vulkan reads one exact RGBA8 rectangle after its graph submission completes", "[renderer][vulkan][graph][color-readback]") {
        using SubmissionTest::Take;
        enum class Crop {
            Valid,
            Outside,
            Oversized,
            WrongStaging,
            LoadWithoutClear,
            AliasedStaging
        };
        const auto scenario =
            GENERATE(Crop::Valid, Crop::Outside, Crop::Oversized, Crop::WrongStaging, Crop::LoadWithoutClear, Crop::AliasedStaging);
        CAPTURE(static_cast<int>(scenario));
        NativeGraphFixture fixture;
        const RenderTextureDescriptor colorDescriptor{.extent = {8, 6},
                                                      .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource};
        const auto colorCost = Take(fixture.resources.QueryTextureMemoryCost(colorDescriptor));
        const auto colorNative = Take(fixture.resources.CreateTexture(colorDescriptor, {}, TestSupport::PlacementFor(colorCost, 1)));
        constexpr std::size_t CropBytes = 24;
        const auto stagingBytes = scenario == Crop::WrongStaging ? CropBytes * 2 : CropBytes;
        const RenderBufferDescriptor stagingDescriptor{stagingBytes, RenderBufferUsage::CopyDestination, RenderBufferAccess::HostVisible};
        const auto stagingCost = Take(fixture.resources.QueryBufferMemoryCost(stagingDescriptor));
        const auto stagingNative = Take(fixture.resources.CreateBuffer(stagingDescriptor, {}, TestSupport::PlacementFor(stagingCost, 2)));

        auto builder = Take(RenderGraphBuilder::Create({2, 3, 3, 2}));
        const auto color = Take(builder.ImportTexture({{1}, 1, 1}, RenderGraphResourceClass::Persistent));
        const auto staging = Take(builder.ImportBuffer({{1}, 2, 1}, RenderGraphResourceClass::Persistent));
        const auto alias = scenario == Crop::AliasedStaging ? Take(builder.ImportBuffer({{1}, 3, 1}, RenderGraphResourceClass::Persistent))
                                                            : RenderGraphResourceId{};
        const auto clear = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto copy = Take(builder.AddPass(RenderPassKind::Copy, RenderQueueRole::Graphics));
        const bool loadOnly = scenario == Crop::LoadWithoutClear;
        REQUIRE(builder
                    .AddUsage({clear, color, loadOnly ? RenderGraphAccess::ReadWrite : RenderGraphAccess::Write,
                               RenderGraphUsageKind::ColorAttachment})
                    .HasValue());
        REQUIRE(builder.AddUsage({copy, color, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource}).HasValue());
        REQUIRE(builder.AddUsage({copy, staging, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination}).HasValue());
        REQUIRE(builder.AddDependency({clear, copy}).HasValue());
        const auto authored = Take(builder.Finalize());
        const auto schedule = Take(CompileRenderGraph(authored));
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        std::vector initial{RenderGraphImportedState{color, {}, RenderGraphInitialContents::Discarded},
                            RenderGraphImportedState{staging, {}, RenderGraphInitialContents::Discarded}};
        if (loadOnly)
            initial.front() = {color,
                               {RenderGraphSynchronizationAccess::Write,
                                RenderGraphSynchronizationOperation::ColorAttachment,
                                RenderGraphPipelineScope::Graphics,
                                RenderGraphTextureLayout::ColorAttachment,
                                {1}},
                               RenderGraphInitialContents::Preserved};
        if (alias.IsValid())
            initial.push_back({alias, {}, RenderGraphInitialContents::Discarded});
        const auto sync = Take(SynthesizeRenderGraphSynchronization(authored, schedule, queues, initial));
        const auto graph = Take(CompileRenderGraphExecution(authored, schedule, sync, queues));
        const auto crop =
            RenderGraphColorReadback{color, staging, scenario == Crop::Outside ? 7U : 1U, 3, {scenario == Crop::Oversized ? 257U : 3U, 2}};
        const auto load = loadOnly ? AttachmentLoadOperation::Load : AttachmentLoadOperation::Clear;
        const std::array workloads{RenderGraphPassWorkload{clear, RenderGraphColorAttachment{color, {.loadOperation = load}}},
                                   RenderGraphPassWorkload{copy, crop}};
        std::vector resources{RenderGraphResourceInstance{color, colorNative}, RenderGraphResourceInstance{staging, stagingNative}};
        if (alias.IsValid())
            resources.push_back({alias, stagingNative});
        const RenderTimelinePoint completion{{1}, 1};
        const std::array submissions{RenderGraphQueueSubmission{{{1}, {1}, {}, completion}, {0, workloads.size()}}};
        RenderGraphLeaseTransfer transfer;
        const RenderGraphExecutionRequest request{{1}, graph, workloads, resources, &fixture.lease, false, &transfer, submissions};
        if (loadOnly || scenario == Crop::AliasedStaging) {
            const auto canonical = ValidateRenderGraphExecutionRequest(request);
            REQUIRE(canonical.HasError());
            CHECK(canonical.ErrorValue().message.find("stored same-graph color attachment") != std::string::npos);
        }
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        const auto recorded = fixture.graph.ExecuteGraph(request);
        if (scenario != Crop::Valid) {
            REQUIRE(recorded.HasError());
            CHECK(fixture.encoded.coverageCopies.empty());
            fixture.graph.AbortFrame({1});
            return;
        }
        INFO((recorded.HasError() ? recorded.ErrorValue().message : "recorded"));
        REQUIRE(recorded.HasValue());
        REQUIRE(fixture.encoded.coverageCopies.size() == 1);
        const auto &encoded = fixture.encoded.coverageCopies.front();
        CHECK(encoded.imageOffset.x == 1);
        CHECK(encoded.imageOffset.y == 3);
        CHECK(encoded.imageExtent.width == 3);
        CHECK(encoded.imageExtent.height == 2);
        CHECK(encoded.imageSubresource.layerCount == 1);
        CHECK(std::ranges::any_of(fixture.encoded.barriers, [](const VkBufferMemoryBarrier2 &barrier) {
            return barrier.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT && barrier.dstAccessMask == VK_ACCESS_2_HOST_READ_BIT &&
                   barrier.dstStageMask == VK_PIPELINE_STAGE_2_HOST_BIT && barrier.size == CropBytes;
        }));
        REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());

        std::array<std::byte, CropBytes> pixels{};
        CHECK_FALSE(Take(fixture.graph.ReadCompletedColor(stagingNative, completion, pixels)));
        fixture.commandState.state.Complete();
        CHECK(fixture.graph.ReadCompletedStereoCoverage(stagingNative, completion, pixels).HasError());
        CHECK(fixture.graph.ReadCompletedColor(stagingNative, completion, std::span{pixels}.first(1)).HasError());
        auto expected = std::span{fixture.resourceState.state.mapped}.first(CropBytes);
        std::ranges::fill(expected, std::byte{0x5a});
        REQUIRE(Take(fixture.graph.ReadCompletedColor(stagingNative, completion, pixels)));
        CHECK(std::ranges::equal(pixels, expected));
        CHECK(fixture.commandState.state.waitCalls == 0);
    }

    TEST_CASE("Vulkan pre-frame uniform preparation waits for exact native readers by polling only",
              "[renderer][vulkan][graph][reconstruction][retirement]") {
        NativeGraphFixture fixture;
        MultiviewPacket packet(fixture, false);
        const auto uniform = packet.parameters;
        constexpr std::size_t UniformBytes = 224;
        std::array<std::byte, UniformBytes> initial;
        initial.fill(std::byte{0x35});
        std::array<std::byte, UniformBytes> next;
        next.fill(std::byte{0x61});
        auto written = std::span{fixture.resourceState.state.mapped}.first(UniformBytes);
        REQUIRE(SubmissionTest::Take(fixture.graph.ObservePreparationBuffer(uniform)));
        CHECK(fixture.graph.ObservePreparationBuffer(0).HasError());
        CHECK(fixture.graph.WritePreparationUniform(packet.native.vertex, initial).HasError());
        CHECK(fixture.graph.WritePreparationUniform(uniform, std::span{initial}.first(1)).HasError());
        REQUIRE(fixture.graph.WritePreparationUniform(uniform, initial).HasValue());
        CHECK(std::ranges::equal(written, initial));

        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        CHECK(fixture.graph.ObservePreparationBuffer(uniform).HasError());
        CHECK(fixture.graph.WritePreparationUniform(uniform, next).HasError());
        CHECK(std::ranges::equal(written, initial));
        RenderGraphLeaseTransfer transfer;
        REQUIRE(fixture.graph.ExecuteGraph(packet.Request(fixture, transfer)).HasValue());
        REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
        CHECK_FALSE(SubmissionTest::Take(fixture.graph.ObservePreparationBuffer(uniform)));
        const auto busy = fixture.graph.WritePreparationUniform(uniform, next);
        REQUIRE(busy.HasError());
        CHECK(busy.ErrorValue().code.Value() == VulkanBackendErrors::SubmissionBusy.code.Value());
        CHECK(std::ranges::equal(written, initial));
        CHECK(fixture.commandState.state.waitCalls == 0);

        fixture.commandState.state.Complete();
        REQUIRE(SubmissionTest::Take(fixture.graph.ObservePreparationBuffer(uniform)));
        REQUIRE(fixture.graph.WritePreparationUniform(uniform, next).HasValue());
        CHECK(std::ranges::equal(written, next));
        REQUIRE(fixture.graph.BeginShutdown().HasValue());
        CHECK(fixture.graph.ObservePreparationBuffer(uniform).HasError());
        CHECK(fixture.graph.WritePreparationUniform(uniform, initial).HasError());
        CHECK(std::ranges::equal(written, next));
        CHECK(fixture.commandState.state.waitCalls == 0);
    }

    TEST_CASE("Vulkan explicit graph admits the bounded scene and color material pass inventory",
              "[renderer][vulkan][graph][color-output]") {
        const bool beyondLimit = GENERATE(false, true);
        NativeGraphFixture fixture;
        const MaterialPacket packet(fixture);
        const auto count = RenderGraphMaterialRaster::MaximumRetainedPasses + (beyondLimit ? 1 : 0);
        auto builder = SubmissionTest::Take(RenderGraphBuilder::Create({count, 1, count, count}));
        const auto color = SubmissionTest::Take(builder.ImportTexture({{1}, 3, 1}, RenderGraphResourceClass::Persistent));
        std::vector<RenderGraphPassWorkload> workloads;
        RenderGraphPassRef previous;
        for (std::size_t index = 0; index < count; ++index) {
            const auto pass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
            REQUIRE(builder.AddUsage({pass, color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}).HasValue());
            if (previous.IsValid())
                REQUIRE(builder.AddDependency({previous, pass, RenderGraphDependencyKind::ExecutionOrder}).HasValue());
            previous = pass;
            workloads.push_back(
                {pass, RenderGraphMaterialRaster{.color = color, .colorView = {{1}, 4, 1}, .extent = {8, 8}, .depthClear = std::nullopt}});
        }
        const auto graph = SubmissionTest::Take(builder.Finalize());
        const auto schedule = SubmissionTest::Take(CompileRenderGraph(graph));
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const std::array initial{RenderGraphImportedState{color, {}, RenderGraphInitialContents::Discarded}};
        const auto sync = SubmissionTest::Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial));
        const auto compiled = SubmissionTest::Take(CompileRenderGraphExecution(graph, schedule, sync, queues));
        const std::array instances{RenderGraphResourceInstance{color, packet.texture}};
        const std::array views{RenderGraphTextureViewInstance{{{1}, 4, 1}, packet.view}};
        const std::array submissions{RenderGraphQueueSubmission{RenderQueueSubmission{{1}, {1}, {}, {{1}, 1}}, {0, count}}};
        RenderGraphLeaseTransfer transfer;
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        const auto execute =
            fixture.graph.ExecuteGraph({{1}, compiled, workloads, instances, &fixture.lease, false, &transfer, submissions, {}, views});
        if (beyondLimit) {
            CHECK(execute.HasError());
            CHECK(fixture.encoded.renderingBegins == 0);
            fixture.graph.AbortFrame({1});
            return;
        }
        REQUIRE(execute.HasValue());
        CHECK(fixture.encoded.renderingBegins == count);
        REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
    }

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

    TEST_CASE("Vulkan canvas upload borrows one exact acquired frame before its first recording",
              "[renderer][vulkan][graph][ui-paint][retirement]") {
        NativeGraphFixture fixture;
        fixture.resourceState.state.sizedAllocations = true;
        constexpr std::array vertices{UiPaintVertex{{1, 1}}, UiPaintVertex{{4, 1}}, UiPaintVertex{{1, 4}}};
        constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
        constexpr std::array parameters{UiPaintParameters{.extent = {8, 8}}};
        constexpr std::array sizes{sizeof(vertices), sizeof(indices), UiPaintFrameUpdate::ParameterStride};
        constexpr std::array roles{RenderBufferUsage::Vertex, RenderBufferUsage::Index, RenderBufferUsage::Uniform};
        NativeUiPaintFrameUpdate pending{.update = {1, vertices, indices, parameters}, .frame = {1}};
        for (std::size_t index = 0; index < roles.size(); ++index) {
            const RenderBufferDescriptor descriptor{.byteSize = sizes[index],
                                                    .usage = roles[index],
                                                    .access = RenderBufferAccess::HostVisible};
            const auto plan = SubmissionTest::Take(fixture.resources.QueryBufferMemoryCost(descriptor));
            pending.instances[index] =
                SubmissionTest::Take(fixture.resources.CreateBuffer(descriptor, {}, TestSupport::PlacementFor(plan, index + 1)));
        }
        CHECK(fixture.graph.UpdateUiPaintFrame(pending).HasError());
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        auto stale = pending;
        stale.frame = {2};
        CHECK(fixture.graph.UpdateUiPaintFrame(stale).HasError());
        REQUIRE(fixture.graph.UpdateUiPaintFrame(pending).HasValue());
        const auto uniform = pending.instances[2];
        CHECK(SubmissionTest::Take(fixture.resources.ReadUiPaintBuffer(uniform)).publication.revision == 1);
        pending.update.revision = 2;

        auto builder = SubmissionTest::Take(RenderGraphBuilder::Create({1, 1, 1, 1}));
        const auto pass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto graph = SubmissionTest::Take(builder.Finalize());
        const auto schedule = SubmissionTest::Take(CompileRenderGraph(graph));
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const auto sync = SubmissionTest::Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, {}));
        const auto compiled = SubmissionTest::Take(CompileRenderGraphExecution(graph, schedule, sync, queues));
        const std::array workloads{RenderGraphPassWorkload{pass, std::monostate{}}};
        const std::array submissions{RenderGraphQueueSubmission{RenderQueueSubmission{{1}, {1}, {}, {{1}, 1}}, {0, 1}}};
        REQUIRE(fixture.graph.ExecuteGraph({{1}, compiled, workloads, {}, nullptr, false, nullptr, submissions}).HasValue());
        CHECK(fixture.graph.UpdateUiPaintFrame(pending).HasError());
        CHECK(SubmissionTest::Take(fixture.resources.ReadUiPaintBuffer(uniform)).publication.revision == 1);
        fixture.graph.AbortFrame({1});
        CHECK(fixture.graph.UpdateUiPaintFrame(pending).HasError());
        CHECK(fixture.commandState.state.waitCalls == 0);
    }

    TEST_CASE("Vulkan graph timing arms one exact signal and clears with its active frame", "[unit][renderer][vulkan][graph][timing]") {
        NativeGraphFixture fixture(false, true);
        fixture.commandState.state.queryResults = {100, 1, 160, 1};
        auto packet = SubmissionTest::CopyGraph();
        fixture.Bind(packet, fixture.CreateBuffers());
        const auto records = packet.Submissions();
        const auto signal = records.front().submission.signal;
        const auto transferSignal = records.back().submission.signal;
        const FrameToken frame{1};
        const FrameToken later{2};
        REQUIRE(fixture.graph.BeginFrame(frame).HasValue());
        CHECK(fixture.graph.ArmGraphGpuDuration(later, signal).HasError());
        const auto unsupported = fixture.graph.ArmGraphGpuDuration(frame, transferSignal);
        REQUIRE(unsupported.HasValue());
        CHECK_FALSE(unsupported.Value());
        REQUIRE(fixture.graph.ArmGraphGpuDuration(frame, signal).Value());
        CHECK(fixture.graph.ArmGraphGpuDuration(frame, signal).HasError());
        fixture.graph.AbortFrame(frame);
        REQUIRE(fixture.graph.BeginFrame(later).HasValue());
        const RenderTimelinePoint unrelated{signal.queue, signal.value + 1};
        REQUIRE(fixture.graph.ArmGraphGpuDuration(later, unrelated).Value());
        RenderGraphLeaseTransfer transfer;
        const RenderGraphExecutionRequest request{later,          packet.graph, packet.workloads, packet.instances,
                                                  &fixture.lease, false,        &transfer,        records};
        REQUIRE(fixture.graph.ExecuteGraph(request).HasError());
        REQUIRE(fixture.graph.ArmGraphGpuDuration(later, signal).Value());
        fixture.graph.DisarmGraphGpuDuration(later, unrelated);
        REQUIRE(fixture.graph.ExecuteGraph(request).HasValue());
        CHECK(fixture.commandState.state.timestampWrites.size() == 2);
        REQUIRE(fixture.graph.SubmitFrame(later).HasValue());
        CHECK(fixture.graph.ObserveGraphGpuDuration(signal, 12).HasError());
        CHECK_FALSE(fixture.graph.ObserveGraphGpuDuration(signal, 11).Value().completed);
        fixture.commandState.state.Complete();
        const auto measured = fixture.graph.ObserveGraphGpuDuration(signal, 11).Value();
        CHECK(measured.completed);
        CHECK(measured.elapsed == std::chrono::nanoseconds{60});
        const auto unmeasured = fixture.graph.ObserveGraphGpuDuration(transferSignal, 11).Value();
        CHECK(unmeasured.completed);
        CHECK_FALSE(unmeasured.elapsed);
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
