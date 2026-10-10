#include "MetalParallelWorkTestSupport.h"

namespace Horo::Render::MetalBackendTests {
    TEST_CASE("Parallel CPU recording and owner acceptance do not retire resident pins before native completion",
              "[unit][renderer][parallel][native][resource][completion]") {
        JobSystem jobs{{.workerCount = 2}};
        ParallelHostFixture fixture;
        fixture.observation->releaseFirst.store(true);
        const RenderBufferDescriptor descriptor{.byteSize = 16,
                                                .usage = RenderBufferUsage::CopySource | RenderBufferUsage::CopyDestination};
        const auto source = fixture.frontend->CreateBuffer(descriptor, {});
        const auto destination = fixture.frontend->CreateBuffer(descriptor, {});
        REQUIRE(source.HasValue());
        REQUIRE(destination.HasValue());
        REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
        auto builder = Test::RequireBuilder();
        const auto pass = Test::RequirePass(builder, RenderPassKind::Copy, RenderQueueRole::Transfer);
        const auto sourceId = Test::RequireResource(builder.ImportBuffer(source.Value().handle, RenderGraphResourceClass::Persistent));
        const auto destinationId =
            Test::RequireResource(builder.ImportBuffer(destination.Value().handle, RenderGraphResourceClass::Persistent));
        Test::RequireUsage(builder, {pass, sourceId, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource});
        Test::RequireUsage(builder, {pass, destinationId, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination});
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Transfer, {1}}};
        const RenderGraphLogicalState stateBefore{RenderGraphSynchronizationAccess::Read,
                                                  RenderGraphSynchronizationOperation::CopySource,
                                                  RenderGraphPipelineScope::Transfer,
                                                  RenderGraphTextureLayout::NotApplicable,
                                                  {1}};
        const std::array imports{RenderGraphImportedState{sourceId, stateBefore}, RenderGraphImportedState{destinationId, stateBefore}};
        auto graph = CompileParallelGraph(builder, queues, imports);
        const std::array workloads{RenderGraphPassWorkload{pass, RenderGraphBufferCopy{sourceId, destinationId, 0, 0, 16}}};
        auto frame = BeginParallelTestFrame(*fixture.frontend);
        REQUIRE(frame.PrepareParallelGraphExecution(jobs, graph, workloads).HasValue());
        REQUIRE(AwaitOwner(frame).HasValue());
        CHECK(fixture.state.resourceDestroyCount == 0);
        REQUIRE(frame.Present().HasValue());
        REQUIRE(fixture.frontend->ReleaseBuffer(source.Value().handle).HasValue());
        REQUIRE(fixture.frontend->ReleaseBuffer(destination.Value().handle).HasValue());
        REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
        CHECK(fixture.state.resourceDestroyCount == 0);
        fixture.observation->nativeComplete = true;
        auto next = fixture.frontend->BeginFrame({2, {64, 64}});
        REQUIRE(next.HasValue());
        auto nextFrame = std::move(next).Value();
        nextFrame.Cancel();
        REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
        CHECK(fixture.state.resourceDestroyCount == 2);
        jobs.Shutdown(ShutdownPolicy::Drain);
    }
}  // namespace Horo::Render::MetalBackendTests
