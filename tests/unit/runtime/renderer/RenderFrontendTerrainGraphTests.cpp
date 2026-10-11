#include "../terrain/TerrainRenderTestSupport.h"
#include "RenderFrontendTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;

    TEST_CASE("Compiled graph frame execution resolves only ready generations owned by its frontend",
              "[renderer][render-graph][frontend]") {
        lifecycleState = {};
        lifecycleState.completeGraphOnPresent = false;
        auto frontend = CreateTrackingFrontend();
        const std::array<std::byte, 16> bytes{};
        const auto created = frontend->CreateBuffer({.byteSize = bytes.size(),
                                                     .usage = RenderBufferUsage::CopySource,
                                                     .access = RenderBufferAccess::HostVisible},
                                                    bytes);
        REQUIRE(created.HasValue());
        auto handle = created.Value().handle;
        SECTION("foreign owner") {
            ++handle.owner.value;
        }
        SECTION("stale generation") {
            ++handle.generation;
        }
        SECTION("live generation") {}
        SECTION("backend exception aborts frame") {
            lifecycleState.frameThrowPoint = FrameThrowPoint::Execute;
        }
        RenderGraphPassRef pass;
        auto execution = CompileImportedBufferGraph(handle, pass);
        const std::array bindings{RenderGraphPassWorkload{pass, std::monostate{}}};
        auto begun = frontend->BeginFrame({1, {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        const auto result = frame.ExecuteGraph(execution, bindings);
        if (handle != created.Value().handle || lifecycleState.frameThrowPoint == FrameThrowPoint::Execute) {
            REQUIRE(result.HasError());
            REQUIRE(lifecycleState.abortCount == 1);
            REQUIRE_FALSE(lifecycleState.frameActive);
            REQUIRE(frame.Present().HasError());
        } else {
            REQUIRE(result.HasValue());
            REQUIRE(lifecycleState.graphResources.size() == 1);
            REQUIRE(lifecycleState.graphResources[0].instance != 0);
            REQUIRE(frontend->ReleaseBuffer(created.Value().handle).HasError());
            REQUIRE(frame.ExecuteGraph(execution, bindings).HasError());
            REQUIRE(frame.Present().HasValue());
            REQUIRE(frontend->ReleaseBuffer(created.Value().handle).HasValue());
            REQUIRE(frontend->ProcessResourceRequests().HasValue());
            REQUIRE(lifecycleState.destroyBufferCount == 0);
            lifecycleState.graphLease->Release();
            lifecycleState.graphLease = nullptr;
            REQUIRE(frontend->ProcessResourceRequests().HasValue());
            REQUIRE(lifecycleState.destroyBufferCount == 1);
        }
    }

    struct TerrainOwnerTestDrain {
        Horo::TerrainRender::TerrainTileResourceOwner &owner;

        ~TerrainOwnerTestDrain() {
            (void)owner.BeginShutdown();
            (void)owner.DrainRetired();
        }
    };

    TEST_CASE("Terrain retirement preserves renderer submission pins after CPU readers release",
              "[terrain][renderer][resource][frontend]") {
        lifecycleState = {};
        lifecycleState.completeGraphOnPresent = false;
        auto frontend = CreateTrackingFrontend();
        Horo::Tests::TerrainRendering::SourceFixture source;
        Horo::TerrainRender::TerrainTileResourceOwner owner(*frontend);
        TerrainOwnerTestDrain drain{owner};
        const auto header = source.Request().header;
        REQUIRE(owner.Stage(source.Capture(), header, 0).HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE_FALSE(owner.Advance(header).Value());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(owner.Advance(header).Value());
        auto reader = owner.Snapshot().Value();
        const auto vertices = reader->tiles[0].vertices.handle;
        RenderGraphPassRef pass;
        auto execution = CompileImportedBufferGraph(vertices, pass);
        const std::array workloads{RenderGraphPassWorkload{pass, std::monostate{}}};
        auto begun = frontend->BeginFrame({1, {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        REQUIRE(frame.ExecuteGraph(execution, workloads).HasValue());
        REQUIRE(frame.Present().HasValue());
        REQUIRE(owner.BeginShutdown().HasValue());
        REQUIRE(owner.DrainRetired().Value() == 0);
        reader.reset();
        REQUIRE(owner.DrainRetired().Value() == 1);
        REQUIRE(owner.IsDrained());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(lifecycleState.destroyMeshCount == 1);
        CHECK(lifecycleState.destroyBufferCount == 1);
        REQUIRE(lifecycleState.graphLease != nullptr);
        lifecycleState.graphLease->Release();
        lifecycleState.graphLease = nullptr;
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(lifecycleState.destroyBufferCount == 2);
    }

    TEST_CASE("Terrain backend realization failure preserves active publication and drains failed allocations",
              "[terrain][renderer][resource][frontend]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        Horo::Tests::TerrainRendering::SourceFixture source;
        Horo::TerrainRender::TerrainTileResourceOwner owner(*frontend);
        TerrainOwnerTestDrain drain{owner};
        const auto first = source.Request().header;
        REQUIRE(owner.Stage(source.Capture(), first, 0).HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE_FALSE(owner.Advance(first).Value());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(owner.Advance(first).Value());
        const auto previous = owner.Snapshot().Value()->tiles[0].mesh.handle;
        const auto next = source.Request(2).header;
        REQUIRE(owner.Stage(source.Capture(2), next, 1).HasValue());
        lifecycleState.failResourceCreation = true;
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        auto failed = owner.Advance(next);
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == "render.test.resource_failed");
        CHECK(owner.Snapshot().Value()->tiles[0].mesh.handle == previous);
        const auto retired = owner.DrainRetired();
        REQUIRE(retired.HasValue());
        REQUIRE(retired.Value() == 1);
        lifecycleState.failResourceCreation = false;
        const auto recovery = source.Request(3).header;
        REQUIRE(owner.Stage(source.Capture(3), recovery, 1).HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE_FALSE(owner.Advance(recovery).Value());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(owner.Advance(recovery).Value());
        const auto recovered = owner.Snapshot().Value()->tiles[0].mesh.handle;
        CHECK(recovered != previous);
        REQUIRE(owner.DrainRetired().HasValue());
        REQUIRE(frontend->ResourceState(recovered).HasValue());
        CHECK(frontend->ResourceState(recovered).Value() == RenderResourceState::Ready);
        REQUIRE(owner.BeginShutdown().HasValue());
        REQUIRE(owner.DrainRetired().Value() == 1);
        REQUIRE(owner.IsDrained());
    }
}  // namespace
