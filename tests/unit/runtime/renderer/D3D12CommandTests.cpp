#include "D3D12CommandTestSupport.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12CommandTests {
    TEST_CASE("D3D12 WARP graph copy executes actual bytes through the legacy barrier baseline", "[renderer][d3d12][commands][warp]") {
        Fixture fixture;
        fixture.Start();
        const auto bytes = Fixture::Bytes();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, bytes);
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto &lease = fixture.NextLease();
        auto packet = CopyPacket::Create(source, destination, lease);
        fixture.ClearDiagnostics();
        const auto before = executeCalls.load();
        REQUIRE(fixture.commands->ExecuteGraph(packet.Request()).HasValue());
        CHECK(executeCalls.load() == before + 1);
        CHECK(packet.transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
        fixture.Await({{1}, 1});
        CHECK(lease.releases == 1);
        const auto readback = fixture.resources->Readback(destination, 0, 32);
        REQUIRE(readback.HasValue());
        CHECK(fixture.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
        fixture.RequireCleanDiagnostics();
        fixture.Drain();
    }

    TEST_CASE("D3D12 WARP rejects retired resources before any valid graph prefix executes", "[renderer][d3d12][commands][warp][failure]") {
        Fixture fixture;
        fixture.Start();
        const auto bytes = Fixture::Bytes();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, bytes);
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        fixture.resources->DestroyBuffer(destination);
        auto &lease = fixture.NextLease();
        auto packet = CopyPacket::Create(source, destination, lease);
        const auto before = executeCalls.load();
        const auto result = fixture.commands->ExecuteGraph(packet.Request());
        Test::RequireError(result, "render.d3d12.command_resource");
        CHECK(executeCalls.load() == before);
        CHECK(packet.transfer.authority == RenderGraphLeaseAuthority::Frontend);
        CHECK(lease.releases == 0);
        fixture.Drain();
    }

    TEST_CASE("D3D12 WARP rejects unsupported external handoff without rewriting the graph or submitting",
              "[renderer][d3d12][commands][warp][barrier]") {
        Fixture fixture;
        fixture.Start();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, Fixture::Bytes());
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto &lease = fixture.NextLease();
        auto packet = CopyPacket::Create(source, destination, lease, {1}, 1, 1, RenderGraphResourceClass::External, true);
        REQUIRE(packet.graph.ExternalReleases().size() == 1);
        const auto before = executeCalls.load();
        Test::RequireError(fixture.commands->ExecuteGraph(packet.Request()), "render.d3d12.command_external_state");
        CHECK(executeCalls.load() == before);
        CHECK(packet.graph.ExternalReleases().size() == 1);
        CHECK(packet.transfer.authority == RenderGraphLeaseAuthority::Frontend);
        CHECK(lease.releases == 0);
        fixture.Drain();
    }

    TEST_CASE("D3D12 WARP enhanced barriers require actual support and never silently select legacy",
              "[renderer][d3d12][commands][warp][barrier]") {
        Fixture fixture;
        auto enhanced = fixture.Create(D3D12BarrierMode::Enhanced);
        if (enhanced.HasError()) {
            Test::RequireError(enhanced, "render.d3d12.enhanced_barriers_unsupported");
            CHECK_FALSE(fixture.commands);
            std::cout << "D3D12 enhanced-barrier evidence: explicitly unsupported\n";
            // A separate explicit Legacy request verifies failed initialization released its owner claim.
            auto legacy = fixture.Create(D3D12BarrierMode::Legacy);
            REQUIRE(legacy.HasValue());
            fixture.commands = std::move(legacy).Value();
            fixture.Drain();
            return;  // Explicit unsupported proof, not execution of an invented enhanced path.
        }
        fixture.commands = std::move(enhanced).Value();
        std::cout << "D3D12 enhanced-barrier evidence: actual enhanced path admitted\n";
        const auto bytes = Fixture::Bytes();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, bytes);
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto packet = CopyPacket::Create(source, destination, fixture.NextLease());
        fixture.ClearDiagnostics();
        REQUIRE(fixture.commands->ExecuteGraph(packet.Request()).HasValue());
        fixture.Await({{1}, 1});
        const auto readback = fixture.resources->Readback(destination, 0, 32);
        REQUIRE(readback.HasValue());
        CHECK(fixture.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
        fixture.RequireCleanDiagnostics();
        fixture.Drain();
    }

    TEST_CASE("D3D12 WARP unused transient instance zero does not reject an admitted resident copy",
              "[renderer][d3d12][commands][warp][boundary]") {
        Fixture fixture;
        fixture.Start();
        const auto bytes = Fixture::Bytes();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, bytes);
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto packet =
            CopyPacket::Create(source, destination, fixture.NextLease(), {1}, 1, 1, RenderGraphResourceClass::Persistent, false, true);
        REQUIRE(packet.resources.size() == 3);
        CHECK(packet.resources.back().instance == 0);
        REQUIRE(ValidateRenderGraphExecutionRequest(packet.Request()).HasValue());
        REQUIRE(fixture.commands->ExecuteGraph(packet.Request()).HasValue());
        fixture.Await({{1}, 1});
        const auto readback = fixture.resources->Readback(destination, 0, 32);
        REQUIRE(readback.HasValue());
        CHECK(fixture.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
        fixture.Drain();
    }

    TEST_CASE("D3D12 WARP command factory rejects a different canonical resource authority", "[renderer][d3d12][commands][warp][failure]") {
        Fixture fixture;
        Fixture foreign;
        const D3D12CommandConfiguration configuration{fixture.resources->Snapshot().deviceIncarnation,
                                                      fixture.bindings,
                                                      D3D12BarrierMode::Legacy,
                                                      {}};
        auto rejected = CreateD3D12Commands(fixture.device.Get(), {fixture.queue.Get(), fixture.compute.Get(), fixture.copy.Get()},
                                            *foreign.resources, configuration);
        Test::RequireError(rejected, "render.d3d12.command_resource");
    }

    TEST_CASE("D3D12 WARP command owner replacement requires fresh resources after actual queue use",
              "[renderer][d3d12][commands][warp][lifecycle]") {
        Fixture fixture;
        fixture.Start();
        Test::RequireError(fixture.Create(), "render.d3d12.command_owner_busy");
        fixture.commands.reset();  // No graph was submitted, so the SAME resource authority can admit a replacement.
        fixture.Start();
        const auto bytes = Fixture::Bytes();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, bytes);
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto packet = CopyPacket::Create(source, destination, fixture.NextLease());
        REQUIRE(fixture.commands->ExecuteGraph(packet.Request()).HasValue());
        fixture.Await({{1}, 1});
        CHECK(packet.lease.releases == 1);
        fixture.Drain();
        fixture.commands.reset();
        Test::RequireError(fixture.Create(), "render.d3d12.command_owner_busy");
        Fixture fresh;
        REQUIRE(fresh.resources->Snapshot().deviceIncarnation != fixture.resources->Snapshot().deviceIncarnation);
        fresh.Start();
        const auto freshSource = fresh.Buffer({32, D3D12Tests::CopyUsage}, bytes);
        const auto freshDestination = fresh.Buffer({32, D3D12Tests::CopyUsage});
        auto next = CopyPacket::Create(freshSource, freshDestination, fresh.NextLease());
        REQUIRE(fresh.commands->ExecuteGraph(next.Request()).HasValue());
        fresh.Await({{1}, 1});  // Repeating the numeric point is valid only in this distinct native resource incarnation.
        const auto readback = fresh.resources->Readback(freshDestination, 0, 32);
        REQUIRE(readback.HasValue());
        CHECK(fresh.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
        fresh.Drain();
    }

    TEST_CASE("D3D12 WARP cancellation before native submission preserves frontend lease and bytes",
              "[renderer][d3d12][commands][warp][cancellation]") {
        Fixture fixture;
        fixture.Start();
        const auto initial = Fixture::Bytes();
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, initial);
        const std::array<std::byte, 32> zero{};
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage}, zero);
        auto &lease = fixture.NextLease();
        auto packet = CopyPacket::Create(source, destination, lease);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto before = executeCalls.load();
        Test::RequireError(fixture.commands->ExecuteGraph(packet.Request(), cancellation.Token()), "render.d3d12.command_cancelled");
        CHECK(executeCalls.load() == before);
        CHECK(packet.transfer.authority == RenderGraphLeaseAuthority::Frontend);
        CHECK(lease.releases == 0);
        const auto readback = fixture.resources->Readback(destination, 0, 32);
        REQUIRE(readback.HasValue());
        CHECK(fixture.Read(readback.Value()) == std::vector<std::byte>(zero.begin(), zero.end()));
        fixture.Drain();
    }

    TEST_CASE("D3D12 WARP submission device removal remains typed and never fabricates completion",
              "[renderer][d3d12][commands][warp][failure]") {
        Fixture fixture;
        ComPtr<ID3D12Device5> removable;
        REQUIRE(SUCCEEDED(fixture.device->QueryInterface(IID_PPV_ARGS(&removable))));
        fixture.Start(D3D12BarrierMode::Legacy, {}, {RemoveAfterExecute});
        const auto source = fixture.Buffer({32, D3D12Tests::CopyUsage}, Fixture::Bytes());
        const auto destination = fixture.Buffer({32, D3D12Tests::CopyUsage});
        auto &lease = fixture.NextLease();
        auto packet = CopyPacket::Create(source, destination, lease);
        const auto before = executeCalls.load();
        Test::RequireError(fixture.commands->ExecuteGraph(packet.Request()), "render.d3d12.device_lost");
        CHECK(executeCalls.load() == before + 1);
        CHECK(packet.transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
        const auto observed = fixture.commands->Observe(fixture.PollPoint({{1}, 1}));
        REQUIRE(observed.HasError());
        CHECK(FAILED(fixture.device->GetDeviceRemovedReason()));
        CHECK(lease.releases <= 1);
        fixture.commands.reset();  // Actual removed-device native disposal; lease storage and Resources still outlive it.
        CHECK(lease.releases == 1);
    }
}  // namespace Horo::Render::D3D12CommandTests
#endif

namespace Horo::Render::D3D12CommandTests {
    TEST_CASE("D3D12 copy fixture admits an unused transient only with explicit frontend authority",
              "[renderer][d3d12][commands][portable][boundary]") {
        Lease lease;
        auto packet = CopyPacket::Create(1, 2, lease, {1}, 1, 1, RenderGraphResourceClass::Persistent, false, true);
        auto request = packet.Request();
        REQUIRE(request.transientResourcesAdmitted);
        REQUIRE(request.resources.size() == 3);
        CHECK(request.resources.back().instance == 0);
        REQUIRE(ValidateRenderGraphExecutionRequest(request).HasValue());
        request.transientResourcesAdmitted = false;
        CHECK(ValidateRenderGraphExecutionRequest(request).HasError());
    }

    TEST_CASE("D3D12 external copy fixture carries an actual external-scope final obligation",
              "[renderer][d3d12][commands][portable][barrier]") {
        Lease lease;
        auto packet = CopyPacket::Create(1, 2, lease, {1}, 1, 1, RenderGraphResourceClass::External, true);
        REQUIRE(packet.graph.ExternalReleases().size() == 1);
        REQUIRE(ValidateRenderGraphExecutionRequest(packet.Request()).HasValue());
        CHECK(lease.releases == 0);
        CHECK(packet.transfer.authority == RenderGraphLeaseAuthority::Frontend);
    }
}  // namespace Horo::Render::D3D12CommandTests
