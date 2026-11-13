#include "D3D12CommandTestSupport.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12CommandTests {
    TEST_CASE("D3D12 WARP allocator capacity and submission pins wait for real fence completion",
              "[renderer][d3d12][commands][warp][lifetime]") {
        Fixture fixture;
        D3D12CommandLimits limits;
        limits.maximumPendingSubmissionsPerQueue = 1;
        fixture.Start(D3D12BarrierMode::Legacy, limits);
        const auto bytes = Fixture::Bytes();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, bytes);
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto &firstLease = fixture.NextLease();
        auto first = CopyPacket::Create(source, destination, firstLease);
        auto &secondLease = fixture.NextLease();
        auto second = CopyPacket::Create(source, destination, secondLease, {1}, 2, 2);
        fixture.SettleUploads();
        QueueGate gate(*fixture.device.Get(), *fixture.queue.Get());
        REQUIRE(fixture.commands->ExecuteGraph(first.Request()).HasValue());
        const auto pending = fixture.commands->Observe(fixture.PollPoint({{1}, 1}));
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(firstLease.releases == 0);
        const auto before = executeCalls.load();
        Test::RequireError(fixture.commands->ExecuteGraph(second.Request()), "render.d3d12.command_capacity");
        CHECK(executeCalls.load() == before);
        CHECK(second.transfer.authority == RenderGraphLeaseAuthority::Frontend);
        gate.Open();
        fixture.Await({{1}, 1});
        CHECK(firstLease.releases == 1);
        REQUIRE(fixture.commands->ExecuteGraph(second.Request()).HasValue());
        fixture.Await({{1}, 2});
        CHECK(secondLease.releases == 1);
        const auto readback = fixture.resources->Readback(destination, 0, 32);
        REQUIRE(readback.HasValue());
        CHECK(fixture.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
        fixture.Drain();
    }

    TEST_CASE("D3D12 WARP exact cross queue waits preserve producer bytes and all submitted lifetimes",
              "[renderer][d3d12][commands][warp][queue]") {
        Fixture fixture;
        fixture.Start();
        const auto bytes = Fixture::Bytes();
        const std::array instances{fixture.Buffer({32, D3D12Tests::CopyUsage}, bytes), fixture.Buffer({32, D3D12Tests::CopyUsage}),
                                   fixture.Buffer({32, D3D12Tests::CopyUsage})};
        auto &lease = fixture.NextLease();
        auto packet = QueuePacket::Create(instances, lease);
        const std::array submissions{RenderGraphQueueSubmission{{{1}, {1}, {}, {{1}, 7}}, {0, 1}},
                                     RenderGraphQueueSubmission{{{3}, {2}, packet.waits, {{3}, 3}}, {1, 1}}};
        REQUIRE(packet.graph.AcquireTransfers().size() == 1);
        fixture.SettleUploads();
        QueueGate gate(*fixture.device.Get(), *fixture.queue.Get());
        fixture.ClearDiagnostics();
        REQUIRE(fixture.commands->ExecuteGraph(packet.Request(submissions)).HasValue());
        CHECK(packet.transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
        const auto pending = fixture.commands->Observe(fixture.PollPoint({{3}, 3}));
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(lease.releases == 0);
        fixture.resources->DestroyBuffer(instances[0]);
        fixture.resources->DestroyBuffer(instances[1]);
        CHECK(fixture.resources->Snapshot().retiringBytes > 0);
        gate.Open();
        fixture.Await({{1}, 7});
        fixture.Await({{3}, 3});
        CHECK(lease.releases == 1);
        const auto readback = fixture.resources->Readback(instances[2], 0, 32);
        REQUIRE(readback.HasValue());
        CHECK(fixture.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
        fixture.RequireCleanDiagnostics();
        fixture.Drain();
    }

    TEST_CASE("D3D12 WARP upload graph and readback interoperate through actual cross-service GPU fences",
              "[renderer][d3d12][commands][warp][interop]") {
        Fixture fixture;
        fixture.Start();
        const auto bytes = Fixture::Bytes();
        QueueGate gate(*fixture.device.Get(), *fixture.queue.Get());
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, bytes);
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto packet = CopyPacket::Create(source, destination, fixture.NextLease(), {3}, 1, 5);
        fixture.ClearDiagnostics();
        REQUIRE(fixture.commands->ExecuteGraph(packet.Request()).HasValue());
        const auto readback = fixture.resources->Readback(destination, 0, 32);
        REQUIRE(readback.HasValue());
        const auto pending = fixture.commands->Observe(fixture.PollPoint({{3}, 5}));
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(packet.lease.releases == 0);
        gate.Open();
        // Readback is already queued; no CPU graph completion wait can accidentally supply the required ordering.
        CHECK(fixture.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
        fixture.Await({{3}, 5});
        CHECK(packet.lease.releases == 1);
        fixture.RequireCleanDiagnostics();
        fixture.Drain();
    }

    TEST_CASE("D3D12 WARP pending shutdown rejects new graphs while retaining issued completion observation",
              "[renderer][d3d12][commands][warp][shutdown]") {
        Fixture fixture;
        fixture.Start();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, Fixture::Bytes());
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto &lease = fixture.NextLease();
        auto packet = CopyPacket::Create(source, destination, lease);
        fixture.SettleUploads();
        QueueGate gate(*fixture.device.Get(), *fixture.queue.Get());
        REQUIRE(fixture.commands->ExecuteGraph(packet.Request()).HasValue());
        REQUIRE(fixture.commands->BeginShutdown().HasValue());
        REQUIRE(fixture.commands->BeginShutdown().HasValue());
        const auto drain = fixture.commands->PollDrain();
        REQUIRE(drain.HasValue());
        CHECK(drain.Value() == RenderBackendDrainProgress::Pending);
        CHECK(lease.releases == 0);
        auto late = CopyPacket::Create(source, destination, fixture.NextLease(), {1}, 2, 2);
        Test::RequireError(fixture.commands->ExecuteGraph(late.Request()), "render.d3d12.command_closed");
        const auto pending = fixture.commands->Observe(fixture.PollPoint({{1}, 1}));
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        gate.Open();
        fixture.Await({{1}, 1});
        fixture.Drain();
        CHECK(lease.releases == 1);
    }

    TEST_CASE("D3D12 WARP stale incarnation and unissued fence points cannot observe another device",
              "[renderer][d3d12][commands][warp][lifecycle]") {
        Fixture fixture;
        fixture.Start();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, Fixture::Bytes());
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto packet = CopyPacket::Create(source, destination, fixture.NextLease());
        REQUIRE(fixture.commands->ExecuteGraph(packet.Request()).HasValue());
        auto stale = fixture.PollPoint({{1}, 1});
        ++stale.expectedDeviceIncarnation;
        Test::RequireError(fixture.commands->Observe(stale), "render.d3d12.timeline_stale");
        Test::RequireError(fixture.commands->Observe(fixture.PollPoint({{1}, 2})), "render.d3d12.timeline_unissued");
        fixture.Await({{1}, 1});
        fixture.Drain();
    }
}  // namespace Horo::Render::D3D12CommandTests
#endif
